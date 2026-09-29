// The endpoint agent: the one thing that runs on the machine being watched.
//
// A controller holds a list of endpoints and connects to each one directly, so the agent only ever
// deals with a single peer at a time and never needs to know the names or addresses of anything
// else on the network. It exposes a port, and every request on that port
// is authenticated against the key this machine minted for itself (PROTOCOL.md 3).
//
// What it is NOT yet: it does not capture or encode a desktop. Until that exists it refuses to
// open a session rather than opening one that would silently never produce a frame, because a
// controller waiting on a black screen has no way to tell "not implemented" from "broken".
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rw/credential.hpp"
#include "rw/protocol.hpp"
#include "rw/stream.hpp"

namespace rw {

struct AgentOptions {
    // Every interface, because the agent is the thing exposed to the network. It authenticates every
    // request, so binding widely is the design rather than an oversight. Note that "authenticated"
    // is not the same as "reachable": a peer on this same host is refused with SelfConnection
    // (see peer_is_this_host), so loopback is not a usable path either.
    std::string bindAddress = "0.0.0.0";
    uint16_t port = 0;
    // Where the machine's own key lives. An agent with no key refuses to start rather than running
    // without a credential: an unauthenticated remote-control port is not a useful default.
    std::string keyFile;
    bool verbose = true;
};

class AgentServer {
public:
    explicit AgentServer(AgentOptions options);

    // Loads the key and binds. Returns false and sets *error rather than throwing, so main() can
    // report a missing key or a taken port as a configuration problem.
    bool start(std::string* error);

    uint16_t port() const { return port_; }
    // The id derived from this machine's key: what the user configures on the controller side.
    const std::string& machineId() const { return machineId_; }

    // Accepts, reads and serves whatever is ready, then returns. Non-blocking, so the daemon can
    // call it on a timer and a test can call it with 0 to step deterministically.
    void run_once(int idle_ms);

    void stop();

    size_t connectionsAccepted() const { return connectionsAccepted_; }
    size_t authFailures() const { return authFailures_; }
    size_t sessionsOpened() const { return sessionsOpened_; }

private:
    struct Peer {
        std::unique_ptr<SocketStream> socket;
        std::unique_ptr<FramedChannel> channel;
        // The peer's address as a dotted quad, captured at accept(). "Is this peer me?" is a
        // question about the connection, not about anything the peer claimed about itself.
        std::string remoteAddress;
        bool greeted = false;
        bool authenticated = false;
    };

    void accept_pending();
    void service_readable();
    void handle(Peer& peer, ReceivedFrame& frame);
    void send(Peer& peer, const Message& message);
    void send_error(Peer& peer, ErrorCode code, const std::string& text);
    // Erases the named peer. Names it rather than popping the back, so that closing one
    // connection cannot drop a different one.
    void drop_peer(Peer& peer, const std::string& why);
    // Compares the credential against this machine's key, in constant time, and refuses a request
    // naming a different machine. Returns an ErrorCode, or an empty optional to mean accepted.
    std::optional<ErrorCode> authenticate(Peer& peer, const OpenSession& request,
                                         const std::string& what);
    // True when `address` is this machine: the loopback interface, or one of this host's own
    // addresses. A remote desktop pointed at the machine it is running on is not a remote desktop,
    // and the agent is the only place that can be sure of the answer -- the controller could be a
    // hand-written client that does not check, and a peer cannot be asked whether it is local.
    bool peer_is_this_host(const std::string& address) const;
    void log(const std::string& line) const;

    AgentOptions options_;
    std::string key_;             // this machine's key, text form, from the key file
    std::vector<uint8_t> keyBytes_;
    std::string machineId_;       // derived from key_; what a controller configures
    SocketStream listener_;
    uint16_t port_ = 0;
    std::vector<std::unique_ptr<Peer>> peers_;
    size_t connectionsAccepted_ = 0;
    size_t authFailures_ = 0;
    size_t sessionsOpened_ = 0;
};

}  // namespace rw
