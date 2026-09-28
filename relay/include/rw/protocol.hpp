// Every message in the wire format, plus framing. See PROTOCOL.md.
//
// This is the C++ half of a three-language contract: the same bytes are produced by
// tools/protocol_vectors.py, which is the normative encoder, and consumed by the mod's Java codec.
// `tests/vectors_test.cpp` checks all three against one generated set, so a field width that
// differs between them fails the build instead of becoming wrong bytes on the wire.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "rw/wire.hpp"

namespace rw {

inline constexpr int kVersionMin = 1;
inline constexpr int kVersionMax = 1;
inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::size_t kInputRecordSize = 12;

enum class Type : uint8_t {
    Hello = 0x01,
    HelloAck = 0x02,
    Error = 0x03,
    OpenSession = 0x04,
    SessionOpened = 0x05,
    CloseSession = 0x06,
    Ping = 0x07,
    Pong = 0x08,
    BytePayload = 0x0E,
    Config = 0x10,
    Frame = 0x11,
    KeyframeRequest = 0x12,
    InputBatch = 0x20,
    AgentHello = 0x30,
    AgentHeartbeat = 0x31,
    AgentStats = 0x32,
};

// Unknown is a relay-side state before HELLO, not a wire value; the other two are the
// values that appear in HELLO.role.
enum class Role : uint8_t { Unknown = 0, Controller = 1, Agent = 2 };
enum class ErrorCode : uint16_t {
    Unspecified = 0,
    UnsupportedVersion = 1,
    AuthFailed = 2,
    NotFound = 3,
    BadMessage = 4,
    LimitExceeded = 5,
    AgentBusy = 6,
    UnsupportedCapture = 7,
};
enum class CloseReason : uint16_t { Client = 0, Agent = 1, Error = 2, Replaced = 3 };
enum class CaptureMode : uint8_t { Unknown = 0, Interactive = 1, Locked = 2 };
enum class Quality : uint8_t { Low = 0, Medium = 1, High = 2, Lossless = 3 };
enum class Codec : uint8_t { H264 = 0, H265 = 1, Av1 = 2, Raw = 3 };
enum class Profile : uint8_t { Baseline = 0, Main = 1, High = 2 };
enum class PixFmt : uint8_t { Yuv420 = 0, Yuv444 = 1 };
enum class InputKind : uint8_t {
    Move = 1,
    ButtonDown = 2,
    ButtonUp = 3,
    Wheel = 4,
    KeyDown = 5,
    KeyUp = 6,
    Text = 7,
};
enum class AgentOs : uint8_t { Windows = 1, Linux = 2, MacOs = 3 };

// Frame header flag: the coded picture is an IDR and needs no prior reference.
inline constexpr uint8_t kFrameFlagKeyframe = 0x01;

// Input modifier bits, in the record's flags byte.
inline constexpr uint8_t kModShift = 0x01;
inline constexpr uint8_t kModCtrl = 0x02;
inline constexpr uint8_t kModAlt = 0x04;
inline constexpr uint8_t kModMeta = 0x08;
inline constexpr uint8_t kModCaps = 0x10;
inline constexpr uint8_t kModRepeat = 0x20;

// Agent capability bits, from AGENT_HELLO.
inline constexpr uint16_t kCapInteractive = 0x0001;
inline constexpr uint16_t kCapLocked = 0x0002;
inline constexpr uint16_t kCapMultiMonitor = 0x0004;
inline constexpr uint16_t kCapConsentGated = 0x0008;

// Encoder bits, from AGENT_HELLO.
inline constexpr uint8_t kEncH264Hw = 0x01;
inline constexpr uint8_t kEncH264Sw = 0x02;
inline constexpr uint8_t kEncAv1Hw = 0x04;

struct Hello {
    uint16_t protocolMin = 0;
    uint16_t protocolMax = 0;
    uint8_t role = 0;
    std::string instanceId;
};

struct HelloAck {
    uint16_t protocol = 0;
    std::string instanceId;
    uint32_t maxMessage = 0;
    uint16_t maxWidth = 0;
    uint16_t maxHeight = 0;
};

struct ErrorMsg {
    uint16_t code = 0;
    std::string message;
};

struct OpenSession {
    std::string agentId;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t quality = 0;
    // The token the agent minted for this controller. Opaque to the relay, which forwards it
    // without reading it; only the named agent decides whether it is acceptable.
    std::vector<uint8_t> credential;
};

struct SessionOpened {
    std::string sessionId;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t capture = 0;
};

struct CloseSession {
    uint16_t reason = 0;
};

struct Ping {
    uint32_t id = 0;
    uint64_t micros = 0;
};

struct Pong {
    uint32_t id = 0;
    uint64_t micros = 0;
};

// Opaque to the relay, which forwards it without reading it. Carries the end-to-end session.
struct BytePayload {
    std::vector<uint8_t> payload;
};

struct Config {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t codec = 0;
    uint8_t profile = 0;
    uint16_t bitrateKbps = 0;
    uint8_t fps = 0;
    uint16_t keyframeInterval = 0;
    uint8_t pixFmt = 0;
};

struct Frame {
    struct Rect {
        int16_t x = 0;
        int16_t y = 0;
        int16_t w = 0;
        int16_t h = 0;
    };

    uint32_t frameIndex = 0;
    uint64_t ptsMicros = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<Rect> rects;
    std::vector<uint8_t> data;

    // Undefined bits are preserved, not dropped, so a frame round-trips and a newer sender can set
    // a bit an older receiver carries through. See PROTOCOL.md 2.1.
    bool is_keyframe() const { return (flags & kFrameFlagKeyframe) != 0; }
    uint8_t flags = 0;
};

struct KeyframeRequest {};

struct InputBatch {
    struct Record {
        uint8_t kind = 0;
        uint8_t flags = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t data = 0;
    };

    std::vector<Record> records;

    std::size_t count() const { return records.size(); }
};

struct AgentHello {
    std::string agentId;
    uint8_t os = 0;
    uint16_t capabilities = 0;
    uint16_t maxWidth = 0;
    uint16_t maxHeight = 0;
    uint8_t encoders = 0;
};

struct AgentHeartbeat {
    uint32_t uptimeSeconds = 0;
    uint16_t activeSessions = 0;
};

struct AgentStats {
    uint32_t framesSent = 0;
    uint32_t framesDropped = 0;
    uint64_t bytesSent = 0;
    uint16_t encodeMsAvg = 0;
    uint16_t bitrateKbps = 0;
    uint8_t fps = 0;
    uint8_t inputQueueDepth = 0;
};

using Body = std::variant<Hello, HelloAck, ErrorMsg, OpenSession, SessionOpened, CloseSession, Ping,
                          Pong, BytePayload, Config, Frame, KeyframeRequest, InputBatch, AgentHello,
                          AgentHeartbeat, AgentStats>;

// A decoded message. `type` is redundant with the body variant and is verified on encode, so a
// mismatch is a loud error rather than a message labelled with the wrong kind.
//
// There is deliberately no `flags` member here. The header's flags byte is owned by the body that
// defines it (Frame::flags), and it is derived via body_flags(). Carrying a second copy is how the
// keyframe bit ends up set in one and clear in the other: encoding writes the header from one and
// decoding fills the other from the same byte, so the round trip still matches and only a consumer
// reading the body sees the difference.
struct Message {
    Type type = Type::Hello;
    Body body = Hello{};
};

// The header flags byte implied by a body. Zero for every type that defines no flags.
uint8_t body_flags(const Body& body);

struct Header {
    Type type = Type::Hello;
    uint8_t flags = 0;
    std::size_t length = 0;
};

// The type the body variant implies. Throws on an unknown tag.
Type type_of(const Body& body);

// Short name, matching the class names the Java codec uses and the vector expectations use.
const char* type_name(Type type);

// Reads a header from the front of a region. `avail` is how many bytes are readable from `off`,
// which is how a stream layer calls this before it has the whole message.
Header parse_header(std::span<const uint8_t> region, std::size_t off = 0);

// Decodes exactly one message occupying the region in full. The region must be consumed
// completely; see PROTOCOL.md 2.1.
Message parse_message(std::span<const uint8_t> region, std::size_t off = 0);

std::vector<uint8_t> encode_message(const Message& message);

// Reads a scalar field by "Class.field" or "field", for the conformance test's expectations. Returns
// nullopt for an unknown key, which the test treats as a failure rather than skipping: a silently
// unrecognised key would make the check look like it passed.
//
// C++ has no reflection to do this reflectively the way the Java check does, so it is an explicit
// table. That is the one place the C++ side repeats the schema by hand; keep it in step with the
// struct fields above.
std::optional<int64_t> read_scalar(const Message& message, const std::string& key);

}  // namespace rw
