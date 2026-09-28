#include "rw/relay.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace rw {
namespace {

bool is_loopback(const std::string& address) {
    return address == "127.0.0.1" || address == "localhost" || address == "::1";
}

std::string hex16(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

Message make(Body body) {
    Message m;
    m.body = std::move(body);
    m.type = type_of(m.body);
    return m;
}

}  // namespace

bool Relay::start(std::string* error) {
    if (!is_loopback(options_.bindAddress) && !options_.allowUnauthenticatedLan) {
        // This is the decision point, not a safety net for missing crypto. There is no credential
        // on HELLO, so anything that can reach this port may pair as a controller and be routed to
        // any endpoint it can name. The flag below is the user saying they accept that on this
        // network; nothing here is protecting them from forgetting.
        if (error != nullptr) {
            *error = "refusing to bind " + options_.bindAddress
                   + ": there is no authentication on this relay, so any host that can reach the "
                     "port can pair and be routed to an endpoint. Bind 127.0.0.1, or pass "
                     "--allow-unauthenticated-lan to accept that on this network.";
        }
        return false;
    }
    auto listener = SocketStream::listen(options_.bindAddress, options_.port, &port_);
    if (!listener) {
        if (error != nullptr) {
            *error = "cannot bind " + options_.bindAddress;
        }
        return false;
    }
    listener_ = std::move(*listener);
    listener_.set_nonblocking();
    log("listening on " + options_.bindAddress + ":" + std::to_string(port_));
    if (options_.port != 0 && port_ != options_.port) {
        log("note: asked for port " + std::to_string(options_.port)
            + ", bound " + std::to_string(port_));
    }
    return true;
}

void Relay::stop() {
    listener_.close();
    connections_.clear();
    endpoints_.clear();
    sessions_.clear();
}

void Relay::log(const std::string& line) const {
    if (options_.verbose) {
        std::cout << "[relay] " << line << "\n";
    }
}

void Relay::accept_pending() {
    while (true) {
        sockaddr_in addr{};
#ifdef _WIN32
        int addrLen = sizeof(addr);
        auto raw = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), &addrLen);
#else
        socklen_t addrLen = sizeof(addr);
        auto raw = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), &addrLen);
#endif
#ifdef _WIN32
        if (raw == INVALID_SOCKET) {
            return;  // nothing more queued
        }
#else
        if (raw < 0) {
            return;  // nothing more queued
        }
#endif
        auto socket = std::make_unique<SocketStream>(static_cast<int>(raw));
        socket->set_nonblocking();
        auto connection = std::make_unique<Connection>();
        connection->channel = std::make_unique<FramedChannel>(*socket);
        connection->socket = std::move(socket);
        connections_.push_back(std::move(connection));
        log("accepted connection " + std::to_string(connections_.size()));
    }
}

void Relay::service_readable() {
    // Index-based: a handler can close a connection and invalidate the reference it was given, so
    // nothing may hold a Connection* across a call that can close something.
    for (size_t i = 0; i < connections_.size();) {
        if (!connections_[i]->socket->has_message_available()) {
            i++;
            continue;
        }
        std::optional<ReceivedFrame> frame;
        std::string failure;
        try {
            frame = connections_[i]->channel->read_frame();
        } catch (const std::exception& e) {
            failure = e.what();
        }
        if (!failure.empty()) {
            close_connection(i, failure);
            continue;  // do not advance: the vector shrank
        }
        if (!frame) {
            close_connection(i, "peer closed");
            continue;
        }
        handle(*connections_[i], *frame);
        i++;
    }
}

void Relay::run_once(int idle_ms) {
    const size_t before = connections_.size();
    size_t serviced = forwardedMessages_;
    accept_pending();
    service_readable();
    if (connections_.size() == before && forwardedMessages_ == serviced && idle_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
    }
}

void Relay::send(Connection& c, const Message& message) {
    c.channel->write_message(message);
}

void Relay::send_error(Connection& c, ErrorCode code, const std::string& text) {
    send(c, make(ErrorMsg{static_cast<uint16_t>(code), text}));
}

size_t Relay::index_of(const Connection& target) const {
    for (size_t i = 0; i < connections_.size(); i++) {
        if (connections_[i].get() == &target) {
            return i;
        }
    }
    return connections_.size();
}

Relay::Connection* Relay::find_agent(const std::string& machineId) {
    for (auto& c : connections_) {
        if (c->role == Role::Agent && c->instanceId == machineId) {
            return c.get();
        }
    }
    return nullptr;
}

Relay::Connection* Relay::find_pending_controller(const std::string& machineId) {
    for (auto& candidate : connections_) {
        if (candidate->role == Role::Controller && candidate->pending.has_value()
            && candidate->pending->machineId == machineId) {
            return candidate.get();
        }
    }
    return nullptr;
}

Relay::Connection* Relay::find_session_partner(Connection& from, const std::string& sessionId) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return nullptr;
    }
    if (it->second.controller >= connections_.size() || it->second.agent >= connections_.size()) {
        return nullptr;
    }
    for (auto& c : connections_) {
        if (c.get() == &from) {
            continue;
        }
        if (c.get() == connections_[it->second.controller].get()
            || c.get() == connections_[it->second.agent].get()) {
            return c.get();
        }
    }
    return nullptr;
}

void Relay::handle(Connection& c, ReceivedFrame& frame) {
    const Message& m = frame.message;

    switch (m.type) {
        case Type::Hello: {
            const auto& hello = std::get<Hello>(m.body);
            if (hello.protocolMax < kVersionMin || hello.protocolMin > kVersionMax) {
                send_error(c, ErrorCode::UnsupportedVersion,
                           "no overlap: this relay speaks " + std::to_string(kVersionMin) + "-"
                               + std::to_string(kVersionMax) + ", you offered "
                               + std::to_string(hello.protocolMin) + "-"
                               + std::to_string(hello.protocolMax));
                return;
            }
            if (hello.role != static_cast<uint8_t>(Role::Controller)
                && hello.role != static_cast<uint8_t>(Role::Agent)) {
                send_error(c, ErrorCode::BadMessage, "unknown role " + std::to_string(hello.role));
                return;
            }
            c.role = static_cast<Role>(hello.role);
            c.instanceId = hello.instanceId;
            send(c, make(HelloAck{static_cast<uint16_t>(kVersionMax), hello.instanceId,
                                  static_cast<uint32_t>(kMaxMessage), 3840, 2160}));
            log(std::string(c.role == Role::Agent ? "agent " : "controller ") + hello.instanceId
                + " paired");
            return;
        }

        case Type::AgentHello: {
            if (c.role != Role::Agent) {
                send_error(c, ErrorCode::BadMessage, "AGENT_HELLO from a non-agent role");
                return;
            }
            const auto& hello = std::get<AgentHello>(m.body);
            // The machine id is derived from the endpoint's key, so a collision is already
            // astronomically unlikely. This is the backstop, not the mechanism: a collision must
            // deny a machine rather than let the registry overwrite the first entry and have
            // find_agent() hand the controller whichever connected first.
            for (const auto& other : connections_) {
                if (other.get() == &c || other->role != Role::Agent) {
                    continue;
                }
                if (other->instanceId == hello.machineId) {
                    send_error(c, ErrorCode::DuplicateEndpoint,
                               "machine id " + hello.machineId
                                   + " is already connected from another endpoint");
                    return;
                }
            }
            c.instanceId = hello.machineId;
            c.label = hello.label;
            endpoints_[hello.machineId] =
                EndpointInfo{hello.machineId, hello.label, hello.os, hello.capabilities,
                             hello.maxWidth, hello.maxHeight, hello.encoders};
            log("endpoint " + hello.label + " (" + hello.machineId + ") registered, capture caps "
                + hex16(hello.capabilities));
            return;
        }

        case Type::OpenSession: {
            if (c.role != Role::Controller) {
                send_error(c, ErrorCode::BadMessage, "OPEN_SESSION from a non-controller role");
                return;
            }
            const auto& open = std::get<OpenSession>(m.body);
            Connection* agent = find_agent(open.machineId);
            if (agent == nullptr) {
                send_error(c, ErrorCode::NotFound, "no endpoint " + open.machineId);
                return;
            }
            // Note what is NOT checked here: whether the agent is already serving a session. The
            // relay used to answer AgentBusy itself, and that leaked -- a caller with no valid
            // credential could learn that a guessed agent id exists and is occupied, by reading the
            // error code. Busy-ness is now the agent's answer, given only after it has accepted the
            // credential, so the only callers who can see it are ones already allowed to ask.
            //
            // The agent allocates the session id and answers with SESSION_OPENED, so nothing has
            // to be added to OPEN_SESSION to carry one. One pending request per agent is enough to
            // match the reply.
            c.pending = open;
            agent->channel->write_raw(frame.raw);
            forwardedMessages_++;
            log("controller " + c.instanceId + " asked for " + open.machineId + " at "
                + std::to_string(open.width) + "x" + std::to_string(open.height)
                + (open.credential.empty() ? " with no credential" : ""));
            return;
        }

        case Type::SessionOpened: {
            if (c.role != Role::Agent) {
                send_error(c, ErrorCode::BadMessage, "SESSION_OPENED from a non-agent role");
                return;
            }
            const auto& opened = std::get<SessionOpened>(m.body);
            Connection* controller = find_pending_controller(c.instanceId);
            if (controller == nullptr) {
                send_error(c, ErrorCode::BadMessage,
                           "SESSION_OPENED with no controller waiting on " + c.instanceId);
                return;
            }
            auto controllerIndex = index_of(*controller);
            c.boundSessionId = opened.sessionId;
            controller->boundSessionId = opened.sessionId;
            controller->pending.reset();
            sessions_[opened.sessionId] =
                BoundSession{controllerIndex, index_of(c),
                             SessionInfo{opened.sessionId, c.instanceId, opened.width, opened.height,
                                         opened.capture, 0}};
            controller->channel->write_raw(frame.raw);
            forwardedMessages_++;
            log("session " + opened.sessionId + " up: " + controller->instanceId + " <-> "
                + c.instanceId);
            return;
        }

        case Type::CloseSession: {
            if (!c.boundSessionId.empty()) {
                unbind_session(c.boundSessionId, "closed by " + c.instanceId, &c);
            }
            return;
        }

        case Type::AgentHeartbeat: {
            // Consumed by the relay: liveness bookkeeping, not forwarded. M2 records nothing; M3
            // will mark the endpoint stale so the controller can be told before it tries.
            return;
        }

        case Type::Error: {
            const auto& err = std::get<ErrorMsg>(m.body);
            log("error from " + c.instanceId + ": " + err.message);

            // An agent refusing a credential arrives with no bound session yet, so the
            // find_session_partner path below would drop it and leave the controller waiting
            // forever. Answer the controller that is waiting on this agent instead.
            if (c.role == Role::Agent) {
                if (Connection* controller = find_pending_controller(c.instanceId)) {
                    // Read what was asked for before clearing it: the message the controller gets
                    // must not be able to tell it apart from a request for a name that is not there.
                    const std::string requested =
                        controller->pending ? controller->pending->machineId : c.instanceId;
                    controller->pending.reset();
                    if (err.code == static_cast<uint16_t>(ErrorCode::AuthFailed)) {
                        // Report "no such endpoint", not "your token was wrong". Otherwise the relay
                        // is an enumeration oracle: a caller with no valid credential can tell an
                        // agent that exists but refused it from one that is not there at all, and
                        // walk a list of guessed machine names. Collapsing the two is the whole
                        // point of the check living here rather than in the agent's reply.
                        send_error(*controller, ErrorCode::NotFound, "no endpoint " + requested);
                        forwardedMessages_++;
                        return;
                    }
                    // Everything else the agent says -- busy, unsupported capture -- is an answer
                    // the agent chose to give after accepting the credential, so it passes through.
                    controller->channel->write_raw(frame.raw);
                    forwardedMessages_++;
                    return;
                }
            }

            if (Connection* partner = find_session_partner(c, c.boundSessionId)) {
                partner->channel->write_raw(frame.raw);
                forwardedMessages_++;
            }
            return;
        }

        default: {
            // Everything else -- Ping, Pong, BytePayload, Config, Frame, KeyframeRequest,
            // InputBatch, AgentStats -- is routed to the session partner untouched. The relay has no
            // business interpreting a video frame, and forwarding the sender's bytes means it cannot
            // corrupt one.
            if (c.boundSessionId.empty()) {
                log("dropping " + std::string(type_name(m.type)) + " from " + c.instanceId
                    + ": no session");
                return;
            }
            Connection* partner = find_session_partner(c, c.boundSessionId);
            if (partner == nullptr) {
                close_connection(index_of(c), "session partner is gone");
                return;
            }
            partner->channel->write_raw(frame.raw);
            forwardedMessages_++;
            return;
        }
    }
}

void Relay::unbind_session(const std::string& sessionId, const std::string& why,
                           Connection* initiator) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return;
    }
    const size_t controllerIndex = it->second.controller;
    const size_t agentIndex = it->second.agent;
    log("session " + sessionId + " down: " + why);

    if (agentIndex < connections_.size()) {
        connections_[agentIndex]->boundSessionId.clear();
    }
    if (controllerIndex < connections_.size()) {
        connections_[controllerIndex]->boundSessionId.clear();
    }
    // Tell the side that did not ask. Notifying the initiator would be telling the peer something it
    // already decided, while the half that has to tear down its capture and input state is left
    // wondering whether anyone is still watching.
    for (size_t index : {controllerIndex, agentIndex}) {
        if (index >= connections_.size()) {
            continue;
        }
        Connection& peer = *connections_[index];
        if (initiator != nullptr && &peer == initiator) {
            continue;
        }
        try {
            send_error(peer, ErrorCode::Unspecified, "session ended: " + why);
        } catch (const std::exception&) {
            // The peer may already be gone; unbind either way.
        }
    }
    sessions_.erase(it);
}

void Relay::close_connection(size_t index, const std::string& why) {
    if (index >= connections_.size()) {
        return;
    }
    Connection& c = *connections_[index];
    log("closing " + (c.instanceId.empty() ? std::string("<unpaired>") : c.instanceId) + ": " + why);

    if (!c.boundSessionId.empty()) {
        // unbind_session indexes into connections_ and may write to a peer, so it has to run before
        // this connection is erased -- and it is told this connection initiated the teardown, so it
        // notifies the other side rather than writing into a socket about to close.
        unbind_session(c.boundSessionId, why, &c);
    }
    if (c.role == Role::Agent) {
        endpoints_.erase(c.instanceId);
    }
    connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::vector<EndpointInfo> Relay::endpoints() const {
    std::vector<EndpointInfo> out;
    out.reserve(endpoints_.size());
    for (const auto& [id, info] : endpoints_) {
        out.push_back(info);
    }
    return out;
}

std::optional<SessionInfo> Relay::sessionForMachine(const std::string& machineId) const {
    for (const auto& entry : sessions_) {
        if (entry.second.info.machineId == machineId) {
            return entry.second.info;
        }
    }
    return std::nullopt;
}

std::optional<SessionInfo> Relay::sessionById(const std::string& sessionId) const {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second.info;
}

}  // namespace rw
