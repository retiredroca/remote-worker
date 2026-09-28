// End-to-end test of the relay over real loopback sockets: an agent registers, a controller pairs,
// a session comes up, a video frame and an input batch cross in both directions unchanged, and the
// session comes down when either side leaves.
//
// This drives the actual Relay::run_once, so it covers accept, framing, dispatch, forwarding and
// teardown. It asserts the bytes that come out the far end are the bytes that went in, because the
// relay's whole job is to not alter them.
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rw/credential.hpp"
#include "rw/protocol.hpp"
#include "rw/relay.hpp"
#include "rw/stream.hpp"

namespace {

int g_failures = 0;

void check_bytes(bool equal, const std::string& what, std::span<const uint8_t> expected,
                 std::span<const uint8_t> actual) {
    if (equal) {
        std::cout << "  ok: " << what << "\n";
        return;
    }
    g_failures++;
    std::cerr << "  - FAIL: " << what << "\n      expected " << expected.size()
              << " bytes, got " << actual.size() << "\n";
    for (std::size_t i = 0; i < expected.size() && i < actual.size(); i++) {
        if (expected[i] != actual[i]) {
            std::cerr << "      first difference at byte " << i << ": expected 0x" << std::hex
                      << static_cast<int>(expected[i]) << ", got 0x"
                      << static_cast<int>(actual[i]) << std::dec << "\n";
            break;
        }
    }
}

void check(bool condition, const std::string& what) {
    if (!condition) {
        g_failures++;
        std::cerr << "  - FAIL: " << what << "\n";
    } else {
        std::cout << "  ok: " << what << "\n";
    }
}

rw::Message make(rw::Body body) {
    rw::Message m;
    m.body = std::move(body);
    m.type = rw::type_of(m.body);
    return m;
}

// A test peer. Non-blocking reads, so `read_frame` returning nullopt means "nothing yet" rather than
// "closed" -- the test drives the relay's loop and then checks what arrived.
class Peer {
public:
    explicit Peer(rw::SocketStream socket) : socket_(std::move(socket)) {
        channel_ = std::make_unique<rw::FramedChannel>(socket_);
        socket_.set_nonblocking();
    }

    void send(const rw::Message& m) { channel_->write_message(m); }

    std::optional<rw::ReceivedFrame> poll() {
        if (!socket_.has_message_available()) {
            return std::nullopt;
        }
        return channel_->read_frame();
    }

    // Runs the relay until this peer has a message of the wanted type, or the budget runs out.
    std::optional<rw::ReceivedFrame> await(rw::Relay& relay, rw::Type wanted) {
        for (int i = 0; i < 200 && g_failures >= 0; i++) {
            if (auto frame = poll()) {
                if (frame->message.type == wanted) {
                    return frame;
                }
                continue;  // a different message (HelloAck, an Error); keep looking
            }
            relay.run_once(0);
        }
        return std::nullopt;
    }

private:
    rw::SocketStream socket_;
    std::unique_ptr<rw::FramedChannel> channel_;
};

// The agent's half of the credential decision, in miniature.
//
// This is the logic M4's real agent implements, kept here because the relay is deliberately not
// allowed to do it: the agent is the only holder of a token, so the check has to live where the
// token lives. The order matters and is the whole point -- check the credential FIRST, and only
// then report busy-ness, so a caller with a wrong token cannot learn that the endpoint exists.
class AgentBehaviour {
public:
    explicit AgentBehaviour(std::vector<std::vector<uint8_t>> keys) : keys_(std::move(keys)) {}

    // Services one OPEN_SESSION if the agent has one waiting, and returns the request it answered so
    // the test can assert on what actually reached the agent. It polls rather than taking the frame
    // as an argument on purpose: a caller that had already drained the agent's queue with
    // await(OPEN_SESSION) would leave nothing here to answer.
    std::optional<rw::OpenSession> service(rw::Relay& relay, Peer& agent,
                                           const std::string& sessionId) {
        // One pass first, or the request is still sitting in the controller's socket: the relay has
        // to read it and forward before there is anything here to answer.
        relay.run_once(0);
        auto frame = agent.poll();
        if (!frame || frame->message.type != rw::Type::OpenSession) {
            return std::nullopt;
        }
        const auto open = std::get<rw::OpenSession>(frame->message.body);
        if (!authorised(open.credential)) {
            agent.send(make(rw::ErrorMsg{
                static_cast<uint16_t>(rw::ErrorCode::AuthFailed),
                "no such endpoint"}));
            relay.run_once(0);
            return open;
        }
        if (!boundSession_.empty()) {
            agent.send(make(rw::ErrorMsg{static_cast<uint16_t>(rw::ErrorCode::AgentBusy),
                                          "already serving " + boundSession_}));
            relay.run_once(0);
            return open;
        }
        boundSession_ = sessionId;
        agent.send(make(rw::SessionOpened{sessionId, open.width, open.height,
                                         static_cast<uint8_t>(rw::CaptureMode::Interactive)}));
        relay.run_once(0);
        return open;
    }

    void unbind() { boundSession_.clear(); }
    bool busy() const { return !boundSession_.empty(); }

private:
    bool authorised(const std::vector<uint8_t>& credential) const {
        for (const auto& key : keys_) {
            if (rw::constant_time_equals(key, credential)) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::vector<uint8_t>> keys_;
    std::string boundSession_;
};

}  // namespace

int main() {
    rw::RelayOptions options;
    options.bindAddress = "127.0.0.1";
    options.verbose = false;
    rw::Relay relay(options);

    std::string error;
    if (!relay.start(&error)) {
        std::cerr << "relay did not start: " << error << "\n";
        return 1;
    }
    const uint16_t port = relay.port();
    std::cout << "relay listening on 127.0.0.1:" << port << "\n";

    // --- a non-loopback bind must be refused, not silently allowed --------------------------
    {
        rw::RelayOptions lan;
        lan.bindAddress = "192.168.1.50";
        lan.port = 0;
        lan.verbose = false;
        rw::Relay guard(lan);
        std::string why;
        check(!guard.start(&why),
              "refuses a non-loopback bind without TLS");
        check(why.find("refusing") != std::string::npos,
              "and says why: " + why.substr(0, 60));
    }

    auto agentSocket = rw::SocketStream::connect_loopback(port);
    auto controllerSocket = rw::SocketStream::connect_loopback(port);
    if (!agentSocket || !controllerSocket) {
        std::cerr << "could not connect to the relay\n";
        return 1;
    }
    relay.run_once(0);
    Peer agent(std::move(*agentSocket));
    Peer controller(std::move(*controllerSocket));

    // --- handshake ---------------------------------------------------------------------------
    agent.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                              static_cast<uint8_t>(rw::Role::Agent), "agent-test-1"}));
    auto agentAck = agent.await(relay, rw::Type::HelloAck);
    check(agentAck.has_value(), "agent receives HelloAck");

    controller.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                                   static_cast<uint8_t>(rw::Role::Controller), "controller-1"}));
    auto controllerAck = controller.await(relay, rw::Type::HelloAck);
    check(controllerAck.has_value(), "controller receives HelloAck");

    // A peer offering a range with no overlap must be told so, not silently ignored.
    {
        auto straySocket = rw::SocketStream::connect_loopback(port);
        relay.run_once(0);
        Peer stray(std::move(*straySocket));
        stray.send(make(rw::Hello{99, 99, static_cast<uint8_t>(rw::Role::Controller), "from-the-future"}));
        auto err = stray.await(relay, rw::Type::Error);
        check(err.has_value(), "a version-mismatched peer gets an Error");
        if (err) {
            check(std::get<rw::ErrorMsg>(err->message.body).code
                      == static_cast<uint16_t>(rw::ErrorCode::UnsupportedVersion),
                  "with code UnsupportedVersion");
        }
    }

    // --- the key, and the identity derived from it ------------------------------------------------
    // Minted before registration, because the agent's machine id IS derived from this key. The agent
    // does not pick a name and have a key attached; it has a key, and that is what it is known by.
    const std::string minted = rw::generate_token();
    std::vector<uint8_t> token;
    if (!rw::parse_token(minted.substr(4), &token)) {
        std::cerr << "FAIL: the generated key did not parse back\n";
        return 1;
    }
    const std::string machineId = rw::machine_id(token);
    const std::string machineLabel = "DESKTOP-TEST";
    check(token.size() == rw::kTokenBytes,
          "a minted key carries " + std::to_string(rw::kTokenBytes) + " bytes");
    check(machineId.size() == 16, "and the machine is known by the id derived from it: " + machineId);
    check(minted.find(machineId) == std::string::npos,
          "the id is derived, not the key itself, so an id seen on the wire is not a credential");

    // --- registration ------------------------------------------------------------------------
    agent.send(make(rw::AgentHello{machineId, machineLabel,
                                   static_cast<uint8_t>(rw::AgentOs::Windows),
                                   rw::kCapInteractive | rw::kCapLocked, 3840, 2160,
                                   rw::kEncH264Hw | rw::kEncH264Sw}));
    relay.run_once(0);
    check(relay.endpoints().size() == 1, "endpoint registered");
    if (!relay.endpoints().empty()) {
        // A copy, not a reference: endpoints() returns a vector by value, and front() hands back a
        // reference into that temporary, which is destroyed at the end of the statement.
        const auto ep = relay.endpoints().front();
        check(ep.machineId == machineId, "registered under the id derived from its key: " + ep.machineId);
        check(ep.label == machineLabel, "and carries its label separately: " + ep.label);
        check((ep.capabilities & rw::kCapLocked) != 0,
              "locked-capture capability is carried through, not assumed by the controller");
        check(ep.maxWidth == 3840 && ep.maxHeight == 2160,
              "endpoint geometry recorded: " + std::to_string(ep.maxWidth) + "x"
                  + std::to_string(ep.maxHeight));
    }

    // --- opening a session against an endpoint that is not there ------------------------------
    controller.send(make(rw::OpenSession{"ffffffffffffffff", 1280, 720,
                                         static_cast<uint8_t>(rw::Quality::Medium), /*credential=*/{}}));
    auto notFound = controller.await(relay, rw::Type::Error);
    check(notFound.has_value(), "OPEN_SESSION for an unknown endpoint gets an Error");
    if (notFound) {
        check(std::get<rw::ErrorMsg>(notFound->message.body).code
                  == static_cast<uint16_t>(rw::ErrorCode::NotFound),
              "with code NotFound");
    }

    // --- a real session ----------------------------------------------------------------------
    // The key was minted before registration, and the machine id derived from it; the controller
    // addresses the machine by that id and presents the key. The relay must not read either.
    AgentBehaviour agentMind({token});

    controller.send(make(rw::OpenSession{machineId, 1920, 1080,
                                         static_cast<uint8_t>(rw::Quality::High), token}));
    // service() drains the agent's queue and answers, so the request is returned rather than awaited
    // separately -- awaiting it first would leave nothing for the agent to reply to.
    auto agentSawOpen = agentMind.service(relay, agent, "s-0000-0001");
    check(agentSawOpen.has_value(), "OPEN_SESSION reached the agent, which answered it");
    if (agentSawOpen) {
        check(agentSawOpen->machineId == machineId && agentSawOpen->width == 1920
                  && agentSawOpen->height == 1080,
              "the forwarded OpenSession kept its fields");
        check(agentSawOpen->credential == token,
              "the credential reached the agent byte for byte, unread by the relay");
    }
    auto controllerSawOpened = controller.await(relay, rw::Type::SessionOpened);
    check(controllerSawOpened.has_value(), "SESSION_OPENED is forwarded to the controller");
    auto session = relay.sessionById("s-0000-0001");
    check(session.has_value(), "relay has the session bound");
    if (session) {
        check(session->machineId == machineId, "session names the machine by its derived id");
        check(relay.sessionForMachine(machineId).has_value(),
              "session is findable by machine id");
    }

    // A second controller must be refused while the agent is busy.
    {
        auto secondSocket = rw::SocketStream::connect_loopback(port);
        relay.run_once(0);
        Peer second(std::move(*secondSocket));
        second.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                                    static_cast<uint8_t>(rw::Role::Controller), "controller-2"}));
        second.await(relay, rw::Type::HelloAck);
        second.send(make(rw::OpenSession{machineId, 800, 600,
                                         static_cast<uint8_t>(rw::Quality::Low), token}));
        check(agentMind.service(relay, agent, "s-0000-0002").has_value(), "the agent answered");
        auto busy = second.await(relay, rw::Type::Error);
        check(busy.has_value(),
              "a second, correctly credentialed session on a busy agent is refused");
        if (busy) {
            check(std::get<rw::ErrorMsg>(busy->message.body).code
                      == static_cast<uint16_t>(rw::ErrorCode::AgentBusy),
              "with code AgentBusy, which only a paired caller can see");
        }
    }

    // --- a video frame, agent to controller, byte for byte ------------------------------------
    std::vector<uint8_t> picture;
    for (int i = 0; i < 4096; i++) {
        picture.push_back(static_cast<uint8_t>(i * 31));
    }
    rw::Frame frame;
    frame.frameIndex = 77;
    frame.ptsMicros = 1234567;
    frame.width = 1920;
    frame.height = 1080;
    frame.flags = rw::kFrameFlagKeyframe;
    frame.rects = {rw::Frame::Rect{0, 0, 1920, 1080}, rw::Frame::Rect{10, 20, 30, 40}};
    frame.data = picture;
    const std::vector<uint8_t> frameBytes = rw::encode_message(make(frame));
    agent.send(make(frame));

    auto gotFrame = controller.await(relay, rw::Type::Frame);
    check(gotFrame.has_value(), "a 4 KiB FRAME crosses the relay");
    if (gotFrame) {
        check_bytes(gotFrame->raw == frameBytes,
                    "the forwarded frame is byte-identical (relay altered nothing)", frameBytes,
                    gotFrame->raw);
        const auto f = std::get<rw::Frame>(gotFrame->message.body);
        check(f.frameIndex == 77 && f.is_keyframe(),
              "frame index and keyframe flag survive: index=" + std::to_string(f.frameIndex)
                  + " flags=0x" + std::to_string(f.flags));
        check(f.rects.size() == 2 && f.rects[1].w == 30 && f.rects[1].h == 40,
              "damage rects survive, with w/h in the right order");
        check(f.data.size() == picture.size() && f.data == picture, "the coded picture is intact");
    }

    // --- input, controller to agent ------------------------------------------------------------
    rw::InputBatch batch;
    batch.records = {rw::InputBatch::Record{static_cast<uint8_t>(rw::InputKind::Move), 0, 960, 540, 0},
                     rw::InputBatch::Record{static_cast<uint8_t>(rw::InputKind::KeyDown),
                                            rw::kModShift, 0, 0, 0x2A}};
    const std::vector<uint8_t> batchBytes = rw::encode_message(make(batch));
    controller.send(make(batch));
    auto gotBatch = agent.await(relay, rw::Type::InputBatch);
    check(gotBatch.has_value(), "an INPUT_BATCH crosses the relay");
    if (gotBatch) {
        check_bytes(gotBatch->raw == batchBytes, "the forwarded input batch is byte-identical",
                    batchBytes, gotBatch->raw);
        const auto b = std::get<rw::InputBatch>(gotBatch->message.body);
        check(b.count() == 2, "both records arrived");
        check(b.records[1].kind == static_cast<uint8_t>(rw::InputKind::KeyDown)
                  && b.records[1].flags == rw::kModShift && b.records[1].data == 0x2A,
              "key event kept its modifier and scancode");
    }

    // --- an unbound message is dropped, not misrouted -------------------------------------------
    {
        auto straySocket = rw::SocketStream::connect_loopback(port);
        relay.run_once(0);
        Peer stray(std::move(*straySocket));
        stray.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                                  static_cast<uint8_t>(rw::Role::Controller), "controller-3"}));
        stray.await(relay, rw::Type::HelloAck);
        stray.send(make(rw::Config{640, 480, 0, 1, 1000, 30, 60, 0}));
        bool sawConfig = false;
        for (int i = 0; i < 20; i++) {
            if (auto f = stray.poll()) {
                if (f->message.type == rw::Type::Config) sawConfig = true;
            }
            relay.run_once(0);
        }
        check(!sawConfig, "a CONFIG from a sessionless controller is not routed anywhere");
    }

    // --- teardown ---------------------------------------------------------------------------------
    check(relay.forwardedMessages() > 0,
          "the relay counted " + std::to_string(relay.forwardedMessages()) + " forwards");
    controller.send(make(rw::CloseSession{static_cast<uint16_t>(rw::CloseReason::Client)}));
    relay.run_once(0);
    agentMind.unbind();
    check(!relay.sessionById("s-0000-0001").has_value(), "CLOSE_SESSION tears the session down");
    {
        auto err = agent.await(relay, rw::Type::Error);
        check(err.has_value(), "the agent is told the session ended");
    }

    // --- a wrong credential must be indistinguishable from a wrong agent name --------------------
    // This is the reason the relay rewrites AuthFailed to NotFound. If the two came back as
    // different codes, a caller with no token at all could walk a list of guessed machine names and
    // learn which of them are real, which is worth more than the screens themselves.
    {
        std::vector<uint8_t> wrongToken = token;
        wrongToken[0] ^= 0xFF;

        auto probeSocket = rw::SocketStream::connect_loopback(port);
        relay.run_once(0);
        Peer probe(std::move(*probeSocket));
        probe.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                                   static_cast<uint8_t>(rw::Role::Controller), "probe-1"}));
        probe.await(relay, rw::Type::HelloAck);

        // (a) an agent that exists, with a token it will not accept
        probe.send(make(rw::OpenSession{machineId, 640, 480,
                                        static_cast<uint8_t>(rw::Quality::Low), wrongToken}));
        check(agentMind.service(relay, agent, "s-0000-0003").has_value(), "the agent answered");
        auto rejected = probe.await(relay, rw::Type::Error);
        check(rejected.has_value(), "a wrong credential is refused");
        uint16_t rejectedCode = rejected ? std::get<rw::ErrorMsg>(rejected->message.body).code : 0;
        check(rejectedCode == static_cast<uint16_t>(rw::ErrorCode::NotFound),
              "... and reported as NotFound, not AuthFailed (code " + std::to_string(rejectedCode)
                  + ")");

        // (b) an agent that does not exist at all
        probe.send(make(rw::OpenSession{"ffffffffffffffff", 640, 480,
                                        static_cast<uint8_t>(rw::Quality::Low), wrongToken}));
        auto absent = probe.await(relay, rw::Type::Error);
        uint16_t absentCode = absent ? std::get<rw::ErrorMsg>(absent->message.body).code : 0;
        check(absentCode == rejectedCode,
              "a wrong token and a wrong agent name return the identical code "
                  + std::to_string(absentCode) + ", so there is no enumeration oracle");

        // (c) no credential at all is refused the same way, not treated as a legacy request
        probe.send(make(rw::OpenSession{machineId, 640, 480,
                                        static_cast<uint8_t>(rw::Quality::Low), /*credential=*/{}}));
        check(agentMind.service(relay, agent, "s-0000-0004").has_value(), "the agent answered");
        auto empty = probe.await(relay, rw::Type::Error);
        uint16_t emptyCode = empty ? std::get<rw::ErrorMsg>(empty->message.body).code : 0;
        check(emptyCode == rejectedCode, "an absent credential is refused the same way");
    }

    // --- what is still unauthenticated, pinned by a test ------------------------------------------
    // The credential gates the *endpoint*, not the relay. HELLO still checks only the protocol
    // range, so any host that can reach the port can still pair as a controller and be routed; the
    // agent is what refuses to serve it, and the relay holds no key that could be used to get
    // around that. A future release that authenticates the relay connection changes the outcome
    // HERE, rather than leaving the gap asserted only in a comment.
    {
        auto strangerSocket = rw::SocketStream::connect_loopback(port);
        relay.run_once(0);
        Peer stranger(std::move(*strangerSocket));
        stranger.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                                      static_cast<uint8_t>(rw::Role::Controller), "not-paired"}));
        check(stranger.await(relay, rw::Type::HelloAck).has_value(),
              "the relay still pairs a caller with no credential (it holds none to check)");
        stranger.send(make(rw::OpenSession{machineId, 640, 480,
                                           static_cast<uint8_t>(rw::Quality::Low), /*credential=*/{}}));
        check(agentMind.service(relay, agent, "s-0000-0005").has_value(), "the agent answered");
        auto err = stranger.await(relay, rw::Type::Error);
        check(err.has_value(),
              "... but the agent refuses to serve it, and the relay cannot overrule that");
    }

    // --- two machines that pick the same id must be refused, not silently merged ----------------
    // The machine id is derived from the endpoint's key, so a collision is already astronomically
    // unlikely. This is the backstop: a collision must deny a machine rather than let the registry
    // overwrite the first entry, so find_agent() cannot hand the controller whichever connected
    // first. Note the twin announces the same id with a *different* key -- which is the whole point,
    // because the key is what makes the identity unforgeable.
    {
        auto twinSocket = rw::SocketStream::connect_loopback(port);
        relay.run_once(0);
        Peer twin(std::move(*twinSocket));
        twin.send(make(rw::Hello{rw::kVersionMin, rw::kVersionMax,
                                 static_cast<uint8_t>(rw::Role::Agent), "twin"}));
        twin.await(relay, rw::Type::HelloAck);
        twin.send(make(rw::AgentHello{machineId, machineLabel, static_cast<uint8_t>(rw::AgentOs::Linux),
                                       rw::kCapInteractive, 1920, 1080, rw::kEncH264Hw}));
        auto clash = twin.await(relay, rw::Type::Error);
        check(clash.has_value(), "a second agent claiming a live machine id is refused");
        if (clash) {
            check(std::get<rw::ErrorMsg>(clash->message.body).code
                      == static_cast<uint16_t>(rw::ErrorCode::DuplicateEndpoint),
                  "with code DuplicateEndpoint, which names the cause instead of guessing");
        }

        // The original must still be the one that answers: the refusal has to leave the registry
        // alone, not evict the machine that was there first.
        controller.send(make(rw::OpenSession{machineId, 640, 480,
                                             static_cast<uint8_t>(rw::Quality::Low), token}));
        auto served = agentMind.service(relay, agent, "s-0000-0006");
        check(served.has_value(), "the first agent still answers for that id");
        controller.await(relay, rw::Type::SessionOpened);
        relay.run_once(0);
        check(relay.sessionById("s-0000-0006").has_value(),
              "and the session bound to it, so the refused twin did not take over");
        agentMind.unbind();
        controller.send(make(rw::CloseSession{static_cast<uint16_t>(rw::CloseReason::Client)}));
        relay.run_once(0);
        agent.await(relay, rw::Type::Error);
    }

    relay.stop();
    std::cout << "\n" << (g_failures == 0 ? "relay e2e OK" : "relay e2e FAILED") << "\n";
    return g_failures == 0 ? 0 : 1;
}
