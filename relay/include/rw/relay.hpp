// The relay: pairs a controller with an endpoint agent and routes between them.
//
// It is a router, not a participant. It decodes just enough of each message to know where it goes,
// then forwards the sender's original bytes, so it never needs to understand a video frame and
// cannot alter one. The controller-to-agent session is a separate TLS channel tunnelled through it
// (PROTOCOL.md 3), so even a compromised relay sees only ciphertext.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rw/protocol.hpp"
#include "rw/stream.hpp"

namespace rw {

struct EndpointInfo {
    // Derived from the endpoint's key; what the registry is keyed on and what a controller asks for.
    std::string machineId;
    // Free text for the user interface. May repeat: nothing looks a machine up by it.
    std::string label;
    uint8_t os = 0;
    uint16_t capabilities = 0;
    uint16_t maxWidth = 0;
    uint16_t maxHeight = 0;
    uint8_t encoders = 0;
};

struct SessionInfo {
    std::string sessionId;
    std::string machineId;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t capture = 0;
    size_t forwardedMessages = 0;
};

struct RelayOptions {
    // Loopback by default. Binding anything else requires TLS: see Relay::start.
    std::string bindAddress = "127.0.0.1";
    uint16_t port = 0;
    // Permits an unauthenticated listener on a non-loopback address. Off unless asked for, and the
    // point of the check in start() is that you have to ask.
    bool allowUnauthenticatedLan = false;
    bool verbose = true;
};

class Relay {
public:
    explicit Relay(RelayOptions options) : options_(std::move(options)) {}

    // Binds and starts listening. Refuses a non-loopback bind without TLS: a LAN is not a trusted
    // network, and a relay that answers unauthenticated would hand any host on the wire a live
    // remote-control path. Returns false and sets *error rather than throwing, so main() can report
    // it as a configuration problem.
    bool start(std::string* error);

    uint16_t port() const { return port_; }

    // Accepts, reads and routes whatever is ready, then returns. One pass, non-blocking, so a test
    // can drive it deterministically and a caller can put it on its own thread or timer.
    // One pass. `idle_ms` is how long to wait when there was nothing to do, so a daemon can
    // call this on a timer and a test can call it with 0 to step deterministically.
    void run_once(int idle_ms);

    void stop();

    size_t connectionCount() const { return connections_.size(); }
    size_t forwardedMessages() const { return forwardedMessages_; }
    std::vector<EndpointInfo> endpoints() const;
    std::optional<SessionInfo> sessionForMachine(const std::string& machineId) const;
    std::optional<SessionInfo> sessionById(const std::string& sessionId) const;

private:
    struct Connection {
        std::unique_ptr<SocketStream> socket;
        std::unique_ptr<FramedChannel> channel;
        Role role = Role::Unknown;
        // The machine id derived from the agent's key. For a controller this is whatever it called
        // itself, which is only used for logging.
        std::string instanceId;
        std::string label;
        // Set on a controller once a session is bound; set on an agent while it serves one.
        std::string boundSessionId;
        // A controller's outstanding OPEN_SESSION, awaiting the agent's SESSION_OPENED.
        std::optional<OpenSession> pending;
    };

    void accept_pending();
    void service_readable();
    void handle(Connection& c, ReceivedFrame& frame);
    void send(Connection& c, const Message& message);
    void send_error(Connection& c, ErrorCode code, const std::string& text);
    void close_connection(size_t index, const std::string& why);
    // `initiator` is whoever caused the teardown, and is the one side NOT notified.
    void unbind_session(const std::string& sessionId, const std::string& why,
                       Connection* initiator);
    Connection* find_agent(const std::string& machineId);
    // The controller holding an unanswered OPEN_SESSION for this agent, if any. Used to route the
    // agent's accept-or-refuse answer back to whoever asked, before any session is bound.
    Connection* find_pending_controller(const std::string& machineId);
    // connections_ holds unique_ptr, so the slot of a connection is found by scanning.
    // That is O(n) over the handful of live connections, and it cannot be wrong the way
    // pointer arithmetic on a vector of unique_ptr would be.
    size_t index_of(const Connection& c) const;
    Connection* find_session_partner(Connection& from, const std::string& sessionId);
    void log(const std::string& line) const;

    RelayOptions options_;
    SocketStream listener_;
    uint16_t port_ = 0;
    std::vector<std::unique_ptr<Connection>> connections_;
    // A bound session: the two connection slots plus what the agent reported when it opened. The
    // indices are into connections_, which is only safe because nothing is added or erased while a
    // message is being handled -- close_connection is the only mutator, and it re-resolves first.
    struct BoundSession {
        size_t controller = 0;
        size_t agent = 0;
        SessionInfo info;
    };

    std::map<std::string, EndpointInfo> endpoints_;
    std::map<std::string, BoundSession> sessions_;
    size_t forwardedMessages_ = 0;
};

}  // namespace rw
