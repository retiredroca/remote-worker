#include "rw/agent.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include "rw/wire.hpp"

// For the accept() calls around SocketStream, which owns the handle itself.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
// netdb.h declares addrinfo, getaddrinfo and freeaddrinfo, used by local_addresses() below to
// resolve this host's own addresses. On Windows those come from ws2tcpip.h instead, and nothing on
// POSIX includes it for you. This is the same missing include that broke stream.cpp once already;
// this file gained POSIX code later and needed its own copy.
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
// gethostname() lives here, and the include is likewise not transitive on Windows -- there it comes
// from winsock2.h, which is why this has to be inside the #else.
#include <unistd.h>
#endif

namespace rw {

namespace {

// What the agent advertises as the largest desktop it will accept. The controller may ask for
// anything; the agent is the one that decides what is actually capturable, so it is the agent's
// number that matters.
constexpr uint16_t kMaxWidth = 3840;
constexpr uint16_t kMaxHeight = 2160;

// Is this dotted quad the loopback interface? 127.0.0.0/8 is entirely loopback, not just 127.0.0.1,
// so comparing the whole first octet is both simpler and more correct than an equality test.
bool is_loopback(const std::string& ipv4) {
    return ipv4.rfind("127.", 0) == 0;
}

// The IPv4 addresses this machine answers to, resolved from its hostname.
//
// This is a best-effort list and deliberately so: a name that does not resolve, or a host with no
// resolvable name, yields an empty list and the caller then falls back to the loopback test alone.
// An unresolvable hostname must not turn into "every peer is me", which would refuse every remote
// session on a machine whose name is not in DNS.
std::vector<std::string> local_addresses() {
    std::vector<std::string> found;
    char host[256] = {0};
    if (::gethostname(host, sizeof(host) - 1) != 0) {
        return found;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;  // the agent only ever listens on IPv4; see SocketStream::listen
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &results) != 0 || results == nullptr) {
        return found;
    }
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        char text[INET_ADDRSTRLEN] = {0};
        auto* in = reinterpret_cast<sockaddr_in*>(entry->ai_addr);
        if (::inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr) {
            found.emplace_back(text);
        }
    }
    ::freeaddrinfo(results);
    return found;
}

}  // namespace

bool AgentServer::peer_is_this_host(const std::string& address) const {
    if (address.empty()) {
        // Could not be determined. Refusing here would break every session on a platform where
        // getpeername fails, so this answers "no" and the session proceeds.
        return false;
    }
    if (is_loopback(address)) {
        return true;
    }
    for (const std::string& own : local_addresses()) {
        if (own == address) {
            return true;
        }
    }
    return false;
}

AgentServer::AgentServer(AgentOptions options) : options_(std::move(options)) {
    if (options_.keyFile.empty()) {
        options_.keyFile = KeyStore::default_path();
    }
}

void AgentServer::log(const std::string& line) const {
    if (options_.verbose) {
        std::cout << "[agent] " << line << "\n" << std::flush;
    }
}

bool AgentServer::start(std::string* error) {
    SocketRuntime::ensure_initialised();

    // A key is required, not optional. Running without one would mean a port that answers every
    // request it is given, which is the opposite of what this is for.
    KeyStore store(options_.keyFile);
    if (!store.load(&key_, error)) {
        *error = "no usable key at " + options_.keyFile +
                 " -- run: remote-worker keygen (add --rotate to replace an existing key)";
        return false;
    }
    if (!parse_key(key_, &keyBytes_)) {
        *error = "the key in " + options_.keyFile + " is not a valid key";
        return false;
    }
    machineId_ = machine_id(key_);

    uint16_t bound = 0;
    auto listener = SocketStream::listen(options_.bindAddress, options_.port, &bound);
    if (!listener) {
        *error = "cannot listen on " + options_.bindAddress + ":" + std::to_string(options_.port);
        return false;
    }
    listener_ = std::move(*listener);
    port_ = bound;
    log("machine id " + machineId_ + " listening on " + options_.bindAddress + ":" +
        std::to_string(port_));
    return true;
}

void AgentServer::stop() {
    peers_.clear();
    listener_.close();
}

void AgentServer::run_once(int idle_ms) {
    accept_pending();
    service_readable();
    if (idle_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
    }
}

void AgentServer::accept_pending() {
    while (true) {
        sockaddr_in addr{};
#ifdef _WIN32
        int addrLen = sizeof(addr);
        auto raw = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (raw == INVALID_SOCKET) {
            return;  // nothing more queued
        }
#else
        socklen_t addrLen = sizeof(addr);
        auto raw = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (raw < 0) {
            return;  // nothing more queued
        }
#endif
        auto socket = std::make_unique<SocketStream>(static_cast<int>(raw));
        socket->set_nonblocking();
        auto peer = std::make_unique<Peer>();
        peer->channel = std::make_unique<FramedChannel>(*socket);
        peer->socket = std::move(socket);
        peer->remoteAddress = peer->socket->remote_address();
        // Read the address for the log line *before* the move. After `std::move(peer)` the pointer
        // is null, and `peer->remoteAddress` would be a null dereference -- which is a crash on the
        // first connection rather than on anything rare.
        const std::string from = peer->remoteAddress;
        peers_.push_back(std::move(peer));
        connectionsAccepted_++;
        log("controller connected from " +
            (from.empty() ? std::string("an unknown address") : from));
    }
}

void AgentServer::service_readable() {
    // Copied, not held: a handler can close a peer, which erases it from peers_.
    std::vector<Peer*> snapshot;
    snapshot.reserve(peers_.size());
    for (auto& peer : peers_) {
        snapshot.push_back(peer.get());
    }
    for (Peer* peer : snapshot) {
        // The peer may have been dropped by an earlier iteration in this same pass.
        bool still_live = std::any_of(peers_.begin(), peers_.end(),
                                      [peer](const std::unique_ptr<Peer>& held) {
                                          return held.get() == peer;
                                      });
        if (!still_live) {
            continue;
        }
        if (!peer->socket->has_message_available()) {
            continue;
        }
        std::optional<ReceivedFrame> frame;
        try {
            frame = peer->channel->read_frame();
        } catch (const ProtocolError& e) {
            send_error(*peer, ErrorCode::BadMessage, e.what());
            drop_peer(*peer, "protocol error: " + std::string(e.what()));
            return;
        }
        if (!frame) {
            drop_peer(*peer, "controller disconnected");
            return;
        }
        handle(*peer, *frame);
    }
}

void AgentServer::handle(Peer& peer, ReceivedFrame& frame) {
    const Message& message = frame.message;
    switch (message.type) {
        case Type::Hello: {
            const auto& hello = std::get<Hello>(message.body);
            if (hello.protocolMax < kVersionMin || hello.protocolMin > kVersionMax) {
                send_error(peer, ErrorCode::UnsupportedVersion,
                           "this agent speaks protocol 1");
                drop_peer(peer, "unsupported protocol version");
                return;
            }
            if (hello.role != static_cast<uint8_t>(Role::Controller)) {
                send_error(peer, ErrorCode::BadMessage, "this is an agent, not a controller");
                drop_peer(peer, "wrong role");
                return;
            }
            peer.greeted = true;
            HelloAck ack;
            ack.protocol = static_cast<uint16_t>(kVersionMax);
            // The controller is told which machine it reached, from the key rather than from
            // anything the controller claimed, so a misconfigured address is visible in the log of
            // the machine that was actually contacted.
            ack.instanceId = machineId_;
            ack.maxMessage = static_cast<uint32_t>(kMaxMessage);
            ack.maxWidth = kMaxWidth;
            ack.maxHeight = kMaxHeight;
            send(peer, Message{Type::HelloAck, ack});
            log("controller " + hello.instanceId + " greeted");
            return;
        }
        case Type::OpenSession: {
            if (!peer.greeted) {
                send_error(peer, ErrorCode::BadMessage, "HELLO first");
                drop_peer(peer, "OPEN_SESSION before HELLO");
                return;
            }
            const auto& request = std::get<OpenSession>(message.body);
            std::optional<ErrorCode> refusal = authenticate(peer, request, "session");
            if (refusal) {
                return;  // authenticate() has already sent the error and dropped the peer.
            }

            // Refuse a controller on this machine. The usual case is Minecraft and the agent
            // sharing a computer, where "connect to 127.0.0.1" would otherwise produce a session
            // showing the desktop you are already looking at -- and would invite someone to install
            // a second copy of the mod to do it.
            //
            // After the credential, not before, for the reason AgentBusy is: an answer that depends
            // only on who is asking is a way to learn the agent's policy without a key. This one
            // says "you are here, and here is refused", which is the agent's business.
            if (peer_is_this_host(peer.remoteAddress)) {
                send_error(peer, ErrorCode::SelfConnection,
                           "this controller is on the machine the agent runs on; a remote session "
                           "needs a different computer");
                log("refused a session from this machine (" + peer.remoteAddress +
                    "); remote means remote");
                drop_peer(peer, "self connection");
                return;
            }
            // The key is good. The session is refused anyway, because there is no capture behind
            // this yet, and a controller left waiting on a screen that will never arrive looks
            // exactly like a broken agent. Saying so is the difference between a diagnosis and a
            // bug report.
            send_error(peer, ErrorCode::UnsupportedCapture,
                       "authenticated, but this agent has no capture backend yet; it cannot serve a "
                       "desktop");
            log("refused a session on " + machineId_ + ": capture is not implemented");
            drop_peer(peer, "capture not implemented");
            return;
        }
        case Type::Ping: {
            if (!peer.authenticated) {
                // Ping is cheap and pre-auth it leaks only liveness, which the port bound already
                // does. Answering it is what lets a controller measure the round trip.
                Pong pong;
                const auto& ping = std::get<Ping>(message.body);
                pong.id = ping.id;
                send(peer, Message{Type::Pong, pong});
                return;
            }
            return;
        }
        case Type::CloseSession:
            log("controller closed the session");
            drop_peer(peer, "session closed");
            return;
        case Type::Error:
            log("controller reported an error: " +
                std::get<ErrorMsg>(message.body).message);
            return;
        default:
            send_error(peer, ErrorCode::BadMessage, "unexpected message from a controller");
            drop_peer(peer, "unexpected message");
            return;
    }
}

std::optional<ErrorCode> AgentServer::authenticate(Peer& peer, const OpenSession& request,
                                                   const std::string& what) {
    // Two separate checks, and the order matters for what the log says. The machine id is not a
    // secret and is not compared in constant time; the credential is, and is compared last so that
    // a misconfigured controller is described as a misconfiguration rather than as a bad key.
    if (request.machineId != machineId_) {
        authFailures_++;
        log("refused " + what + ": asked for machine " + request.machineId + ", this is " +
            machineId_);
        send_error(peer, ErrorCode::AuthFailed,
                   "this agent is machine " + machineId_ + ", not " + request.machineId);
        drop_peer(peer, "wrong machine id");
        return ErrorCode::AuthFailed;
    }
    if (request.credential.empty()) {
        authFailures_++;
        log("refused " + what + ": no key supplied");
        send_error(peer, ErrorCode::AuthFailed, "no key supplied");
        drop_peer(peer, "no credential");
        return ErrorCode::AuthFailed;
    }
    if (!constant_time_equals(request.credential, keyBytes_)) {
        authFailures_++;
        log("refused " + what + ": the key did not match");
        send_error(peer, ErrorCode::AuthFailed, "the key did not match");
        drop_peer(peer, "bad credential");
        return ErrorCode::AuthFailed;
    }
    peer.authenticated = true;
    return std::nullopt;
}

void AgentServer::send(Peer& peer, const Message& message) {
    try {
        peer.channel->write_message(message);
    } catch (const std::exception& e) {
        log(std::string("write failed: ") + e.what());
    }
}

void AgentServer::send_error(Peer& peer, ErrorCode code, const std::string& text) {
    ErrorMsg error;
    error.code = static_cast<uint16_t>(code);
    error.message = text;
    send(peer, Message{Type::Error, error});
}

void AgentServer::drop_peer(Peer& peer, const std::string& why) {
    // Erase the peer that was actually named. Popping the back would drop a different connection
    // whenever more than one controller is connected, which is exactly the case where it matters.
    auto it = std::find_if(peers_.begin(), peers_.end(),
                           [&peer](const std::unique_ptr<Peer>& held) {
                               return held.get() == &peer;
                           });
    if (it == peers_.end()) {
        return;  // already gone
    }
    log(why);
    peers_.erase(it);
}

}  // namespace rw
