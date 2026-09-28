// Tests the endpoint agent against a real controller peer over loopback.
//
// The agent is where authentication now happens: with the relay retired there is nothing in front
// of it, so a bug here is a bug in the only thing standing between a LAN host and a session. The
// test therefore drives the actual socket and the actual codec rather than calling the handler, and
// it writes a real key file so the agent's own "read my key" path is what is under test.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "rw/agent.hpp"
#include "rw/credential.hpp"
#include "rw/protocol.hpp"
#include "rw/stream.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    std::cout << (ok ? "  ok: " : "  - FAIL: ") << what << "\n";
    if (!ok) {
        g_failures++;
    }
}

// Puts the agent through a few run_once passes so a message written in one call can be read in the
// next. The agent is non-blocking, so a single pass can stop between "accepted" and "readable".
void settle(rw::AgentServer& agent) {
    for (int i = 0; i < 8; i++) {
        agent.run_once(0);
    }
}

// A client speaking the controller half of the protocol.
class Controller {
public:
    bool open(rw::AgentServer& agent) {
        stream_ = rw::SocketStream::connect_loopback(agent.port());
        if (!stream_) {
            return false;
        }
        channel_ = std::make_unique<rw::FramedChannel>(*stream_);
        return true;
    }

    rw::Message greet(const std::string& instanceId = "controller-1") {
        return greet_version(static_cast<uint16_t>(rw::kVersionMin),
                             static_cast<uint16_t>(rw::kVersionMax), instanceId);
    }

    // Greeting with an explicit protocol range, so the version refusal can be tested.
    rw::Message greet_version(uint16_t min, uint16_t max, const std::string& instanceId) {
        rw::Hello hello;
        hello.protocolMin = min;
        hello.protocolMax = max;
        hello.role = static_cast<uint8_t>(rw::Role::Controller);
        hello.instanceId = instanceId;
        rw::Message message{rw::Type::Hello, hello};
        channel_->write_message(message);
        return message;
    }

    rw::Message open_session(const std::string& machineId, const std::string& key) {
        rw::OpenSession request;
        request.machineId = machineId;
        request.width = 1920;
        request.height = 1080;
        request.quality = static_cast<uint8_t>(rw::Quality::High);
        // The controller is given the key as the user sees it, prefix and all, and decodes
        // it the way the mod does before putting the key on the wire.
        rw::parse_key(key, &request.credential);
        rw::Message message{rw::Type::OpenSession, request};
        channel_->write_message(message);
        return message;
    }

    // A Ping, which the agent answers before authentication: a controller needs to measure the
    // round trip to tell a dead endpoint from a slow one.
    rw::Message ping(uint32_t id) {
        rw::Message message{rw::Type::Ping, rw::Ping{id, 0}};
        channel_->write_message(message);
        return message;
    }

    std::optional<rw::ReceivedFrame> read() {
        try {
            return channel_->read_frame();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    void close() {
        channel_.reset();
        stream_.reset();
    }

private:
    std::optional<rw::SocketStream> stream_;
    std::unique_ptr<rw::FramedChannel> channel_;
};

std::optional<rw::ErrorMsg> expect_error(Controller& controller) {
    auto frame = controller.read();
    if (!frame || frame->message.type != rw::Type::Error) {
        return std::nullopt;
    }
    return std::get<rw::ErrorMsg>(frame->message.body);
}

// Starts an agent on loopback with a freshly minted key, writing the key to a temp file.
struct Fixture {
    std::filesystem::path dir;
    std::string keyFile;
    std::string key;
    rw::AgentServer agent{rw::AgentOptions{"127.0.0.1", 0, "", true}};

    bool start() {
        dir = std::filesystem::temp_directory_path() /
              ("remote-worker-agent-test-" + std::to_string(::rand()));
        std::filesystem::create_directories(dir);
        keyFile = (dir / "agent.key").string();
        key = rw::generate_token();
        std::string error;
        rw::KeyStore store(keyFile);
        if (!store.store(key, &error)) {
            std::cerr << "cannot write the test key: " << error << "\n";
            return false;
        }
        rw::AgentOptions options;
        options.bindAddress = "127.0.0.1";
        options.port = 0;
        options.keyFile = keyFile;
        options.verbose = false;
        agent = rw::AgentServer(options);
        if (!agent.start(&error)) {
            // Reported rather than swallowed: a fixture that cannot start would otherwise look like
            // a test failure with no cause.
            std::cerr << "could not start the test agent: " << error << "\n";
            return false;
        }
        return true;
    }

    void cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
};

}  // namespace

int main() {
    std::cout << "agent test\n";

    // A key file is mandatory. An agent that would start without one would be an unauthenticated
    // remote-control port, so this is the first thing checked.
    {
        std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "remote-worker-definitely-no-such-key";
        std::error_code ignored;
        std::filesystem::remove(missing, ignored);
        rw::AgentServer agent(rw::AgentOptions{"127.0.0.1", 0, missing.string(), false});
        std::string error;
        check(!agent.start(&error),
              "an agent with no key file refuses to start, rather than serving without a credential");
        check(error.find("keygen") != std::string::npos,
              "and the error says how to fix it: " + error);
    }

    Fixture fixture;
    if (!fixture.start()) {
        std::cerr << "could not start the test agent\n";
        return 1;
    }
    const std::string machineId = rw::machine_id(fixture.key);
    check(fixture.agent.machineId() == machineId,
          "the agent reports the id derived from its own key: " + machineId);
    settle(fixture.agent);

    // A wrong key is refused, and the machine id alone does not get in: it travels in the open.
    {
        Controller controller;
        check(controller.open(fixture.agent), "a controller can connect");
        controller.greet();
        settle(fixture.agent);
        auto ack = controller.read();
        check(ack && ack->message.type == rw::Type::HelloAck, "HELLO is answered with HELLO_ACK");
        if (ack && ack->message.type == rw::Type::HelloAck) {
            const auto& body = std::get<rw::HelloAck>(ack->message.body);
            check(body.instanceId == machineId,
                  "the ack names the machine that was actually reached, from its key");
        }

        // The right machine id, the wrong key: the id is public, so it must not be enough.
        controller.open_session(machineId, rw::generate_token());
        settle(fixture.agent);
        auto error = expect_error(controller);
        check(error && error->code == static_cast<uint16_t>(rw::ErrorCode::AuthFailed),
              "the right machine id with the wrong key is refused, because the id is not a secret");
        check(fixture.agent.authFailures() == 1, "and it is counted as an auth failure");
        controller.close();
        settle(fixture.agent);
    }

    // A wrong machine id is reported as a misconfiguration, naming both machines.
    {
        Controller controller;
        controller.open(fixture.agent);
        controller.greet();
        settle(fixture.agent);
        controller.read();
        controller.open_session(rw::machine_id(rw::generate_token()), fixture.key);
        settle(fixture.agent);
        auto error = expect_error(controller);
        check(error && error->code == static_cast<uint16_t>(rw::ErrorCode::AuthFailed),
              "the right key for the wrong machine is refused");
        check(error && error->message.find(machineId) != std::string::npos,
              "and the refusal says which machine this actually is");
        controller.close();
        settle(fixture.agent);
    }

    // The good case: authenticated, and then refused for the honest reason.
    {
        Controller controller;
        controller.open(fixture.agent);
        controller.greet();
        settle(fixture.agent);
        controller.read();
        controller.open_session(machineId, fixture.key);
        settle(fixture.agent);
        auto error = expect_error(controller);
        check(error.has_value(), "the correct key is accepted rather than refused");
        check(error && error->code == static_cast<uint16_t>(rw::ErrorCode::UnsupportedCapture),
              "then the session is refused with UnsupportedCapture, because there is no capture "
              "backend yet");
        check(error && error->message.find("no capture backend") != std::string::npos,
              "and the message says why, instead of leaving a controller on a black screen");
        check(fixture.agent.authFailures() == 2,
              "a correct key is not counted as an auth failure, so the count means what it says");
        check(fixture.agent.sessionsOpened() == 0,
              "and no session is recorded as open, because none was");
        controller.close();
        settle(fixture.agent);
    }

    // Ping is answered without a key, so a controller can tell a dead endpoint from a refusing one.
    {
        Controller controller;
        controller.open(fixture.agent);
        controller.ping(7);
        settle(fixture.agent);
        auto frame = controller.read();
        check(frame && frame->message.type == rw::Type::Pong, "PING is answered with PONG");
        if (frame && frame->message.type == rw::Type::Pong) {
            check(std::get<rw::Pong>(frame->message.body).id == 7,
                  "and the PONG carries the id that was asked for, so it can be matched up");
        }
        controller.close();
        settle(fixture.agent);
    }

    // A version the agent does not speak is refused rather than half-understood.
    {
        Controller controller;
        controller.open(fixture.agent);
        controller.greet_version(99, 99, "from-the-future");
        settle(fixture.agent);
        auto error = expect_error(controller);
        check(error && error->code == static_cast<uint16_t>(rw::ErrorCode::UnsupportedVersion),
              "a controller speaking a protocol this agent does not is told so");
        controller.close();
        settle(fixture.agent);
    }

    fixture.cleanup();
    std::cout << (g_failures == 0 ? "agent OK" : "agent FAILED") << " (" << g_checks
              << " checks, " << g_failures << " failed)\n";
    return g_failures == 0 ? 0 : 1;
}
