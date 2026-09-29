#include "rw/protocol.hpp"

#include <array>
#include <type_traits>

namespace rw {
namespace {

// Variant order must match this table; type_of and parse_message both index off it.
constexpr std::array<Type, 16> kTypeByBody = {
    Type::Hello,          Type::HelloAck,      Type::Error,        Type::OpenSession,
    Type::SessionOpened,  Type::CloseSession,  Type::Ping,         Type::Pong,
    Type::BytePayload,    Type::Config,        Type::Frame,        Type::KeyframeRequest,
    Type::InputBatch,     Type::AgentHello,    Type::AgentHeartbeat, Type::AgentStats,
};

std::string hex8(uint8_t v) {
    static const char* digits = "0123456789abcdef";
    std::string s = "0x";
    s += digits[(v >> 4) & 0xF];
    s += digits[v & 0xF];
    return s;
}

Body make_body(Type type) {
    switch (type) {
        case Type::Hello: return Hello{};
        case Type::HelloAck: return HelloAck{};
        case Type::Error: return ErrorMsg{};
        case Type::OpenSession: return OpenSession{};
        case Type::SessionOpened: return SessionOpened{};
        case Type::CloseSession: return CloseSession{};
        case Type::Ping: return Ping{};
        case Type::Pong: return Pong{};
        case Type::BytePayload: return BytePayload{};
        case Type::Config: return Config{};
        case Type::Frame: return Frame{};
        case Type::KeyframeRequest: return KeyframeRequest{};
        case Type::InputBatch: return InputBatch{};
        case Type::AgentHello: return AgentHello{};
        case Type::AgentHeartbeat: return AgentHeartbeat{};
        case Type::AgentStats: return AgentStats{};
    }
    throw ProtocolError("unknown message type " + hex8(static_cast<uint8_t>(type)));
}

void encode_body(WireWriter& w, const Message& m) {
    std::visit(
        [&w](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Hello>) {
                w.u16(b.protocolMin).u16(b.protocolMax).u8(b.role).string(b.instanceId);
            } else if constexpr (std::is_same_v<T, HelloAck>) {
                w.u16(b.protocol).string(b.instanceId).u32(b.maxMessage).u16(b.maxWidth)
                    .u16(b.maxHeight);
            } else if constexpr (std::is_same_v<T, ErrorMsg>) {
                w.u16(b.code).string(b.message);
            } else if constexpr (std::is_same_v<T, OpenSession>) {
                w.string(b.machineId).u16(b.width).u16(b.height).u8(b.quality)
                    .bytes_field(b.credential);
            } else if constexpr (std::is_same_v<T, SessionOpened>) {
                w.string(b.sessionId).u16(b.width).u16(b.height).u8(b.capture);
            } else if constexpr (std::is_same_v<T, CloseSession>) {
                w.u16(b.reason);
            } else if constexpr (std::is_same_v<T, Ping>) {
                w.u32(b.id).u64(b.micros);
            } else if constexpr (std::is_same_v<T, Pong>) {
                w.u32(b.id).u64(b.micros);
            } else if constexpr (std::is_same_v<T, BytePayload>) {
                w.bytes_field(b.payload);
            } else if constexpr (std::is_same_v<T, Config>) {
                w.u16(b.width).u16(b.height).u8(b.codec).u8(b.profile).u16(b.bitrateKbps)
                    .u8(b.fps).u16(b.keyframeInterval).u8(b.pixFmt);
            } else if constexpr (std::is_same_v<T, Frame>) {
                if (b.rects.empty()) {
                    throw std::invalid_argument("FRAME rect_count must be at least 1");
                }
                if (b.data.empty()) {
                    throw std::invalid_argument("FRAME picture must not be empty");
                }
                w.u32(b.frameIndex).u64(b.ptsMicros).u16(b.width).u16(b.height)
                    .u16(static_cast<uint16_t>(b.rects.size()));
                for (const auto& r : b.rects) {
                    w.i16(r.x).i16(r.y).i16(r.w).i16(r.h);
                }
                w.bytes_field(b.data);
            } else if constexpr (std::is_same_v<T, KeyframeRequest>) {
                // empty payload
            } else if constexpr (std::is_same_v<T, InputBatch>) {
                if (b.records.empty()) {
                    throw std::invalid_argument("INPUT_BATCH count must be at least 1");
                }
                w.u16(static_cast<uint16_t>(b.records.size()));
                for (const auto& r : b.records) {
                    w.u8(r.kind).u8(r.flags).u16(0).i32(r.x).i32(r.y).u32(r.data);
                }
            } else if constexpr (std::is_same_v<T, AgentHello>) {
                w.string(b.machineId).string(b.label).u8(b.os).u16(b.capabilities).u16(b.maxWidth)
                    .u16(b.maxHeight)
                    .u8(b.encoders);
            } else if constexpr (std::is_same_v<T, AgentHeartbeat>) {
                w.u32(b.uptimeSeconds).u16(b.activeSessions);
            } else if constexpr (std::is_same_v<T, AgentStats>) {
                w.u32(b.framesSent).u32(b.framesDropped).u64(b.bytesSent).u16(b.encodeMsAvg)
                    .u16(b.bitrateKbps).u8(b.fps).u8(b.inputQueueDepth);
            }
        },
        m.body);
}

}  // namespace

Type type_of(const Body& body) {
    const std::size_t idx = body.index();
    if (idx >= kTypeByBody.size()) {
        throw std::invalid_argument("body variant index out of range");
    }
    return kTypeByBody[idx];
}

uint8_t body_flags(const Body& body) {
    return std::visit([](const auto& b) -> uint8_t {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, Frame>) {
            return b.flags;
        } else {
            return 0;
        }
    }, body);
}

const char* type_name(Type type) {
    switch (type) {
        case Type::Hello: return "Hello";
        case Type::HelloAck: return "HelloAck";
        case Type::Error: return "Error";
        case Type::OpenSession: return "OpenSession";
        case Type::SessionOpened: return "SessionOpened";
        case Type::CloseSession: return "CloseSession";
        case Type::Ping: return "Ping";
        case Type::Pong: return "Pong";
        case Type::BytePayload: return "BytePayload";
        case Type::Config: return "Config";
        case Type::Frame: return "Frame";
        case Type::KeyframeRequest: return "KeyframeRequest";
        case Type::InputBatch: return "InputBatch";
        case Type::AgentHello: return "AgentHello";
        case Type::AgentHeartbeat: return "AgentHeartbeat";
        case Type::AgentStats: return "AgentStats";
    }
    return "Unknown";
}

Header parse_header(std::span<const uint8_t> region, std::size_t off) {
    if (region.size() - off < kHeaderSize) {
        throw ProtocolError("header needs " + std::to_string(kHeaderSize) + " bytes");
    }
    auto typeByte = region[off];
    uint16_t reserved = static_cast<uint16_t>(region[off + 2])
                        | static_cast<uint16_t>(static_cast<uint16_t>(region[off + 3]) << 8);
    if (reserved != 0) {
        throw ProtocolError("reserved must be 0, got " + std::to_string(reserved));
    }
    uint32_t length = 0;
    for (int i = 0; i < 4; i++) {
        length |= static_cast<uint32_t>(region[off + 4 + i]) << (8 * i);
    }
    if (length > kMaxMessage) {
        throw ProtocolError("length " + std::to_string(length) + " exceeds MAX_MESSAGE");
    }
    if (length > region.size() - off - kHeaderSize) {
        throw ProtocolError("length " + std::to_string(length) + " exceeds "
                            + std::to_string(region.size() - off - kHeaderSize)
                            + " available bytes");
    }
    return Header{static_cast<Type>(typeByte), region[off + 1], length};
}

Message parse_message(std::span<const uint8_t> region, std::size_t off) {
    Header header = parse_header(region, off);
    // The region must be exactly one message, not merely start with one. Without this, a buffer
    // holding a valid message followed by anything else decodes the first and silently ignores the
    // rest, so a caller validating a received region this way would accept a message whose
    // declared length did not account for what it was handed.
    if (header.length != region.size() - off - kHeaderSize) {
        throw ProtocolError("message occupies " + std::to_string(kHeaderSize + header.length) + " of "
                            + std::to_string(region.size() - off) + " bytes");
    }
    WireReader r(region.subspan(off + kHeaderSize, header.length));
    Message m;
    m.type = header.type;
    m.body = make_body(header.type);
    // The body's view of the header flags. A message that carries flags in its body has to be given
    // them here, or Frame::is_keyframe() reports false on a frame that is one -- while the round
    // trip still succeeds, because encode_message writes Message::flags. Two sources of truth for
    // one bit, disagreeing, and only a consumer reading the body ever notices.
    const uint8_t headerFlags = header.flags;

    std::visit(
        [&r, headerFlags](auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Hello>) {
                b.protocolMin = r.u16();
                b.protocolMax = r.u16();
                b.role = r.u8();
                b.instanceId = r.string();
            } else if constexpr (std::is_same_v<T, HelloAck>) {
                b.protocol = r.u16();
                b.instanceId = r.string();
                b.maxMessage = r.u32();
                b.maxWidth = r.u16();
                b.maxHeight = r.u16();
            } else if constexpr (std::is_same_v<T, ErrorMsg>) {
                b.code = r.u16();
                b.message = r.string();
            } else if constexpr (std::is_same_v<T, OpenSession>) {
                b.machineId = r.string();
                b.width = r.u16();
                b.height = r.u16();
                b.quality = r.u8();
                auto cred = r.bytes_field();
                b.credential.assign(cred.begin(), cred.end());
            } else if constexpr (std::is_same_v<T, SessionOpened>) {
                b.sessionId = r.string();
                b.width = r.u16();
                b.height = r.u16();
                b.capture = r.u8();
            } else if constexpr (std::is_same_v<T, CloseSession>) {
                b.reason = r.u16();
            } else if constexpr (std::is_same_v<T, Ping>) {
                b.id = r.u32();
                b.micros = r.u64();
            } else if constexpr (std::is_same_v<T, Pong>) {
                b.id = r.u32();
                b.micros = r.u64();
            } else if constexpr (std::is_same_v<T, BytePayload>) {
                auto raw = r.bytes_field();
                b.payload.assign(raw.begin(), raw.end());
            } else if constexpr (std::is_same_v<T, Config>) {
                b.width = r.u16();
                b.height = r.u16();
                b.codec = r.u8();
                b.profile = r.u8();
                b.bitrateKbps = r.u16();
                b.fps = r.u8();
                b.keyframeInterval = r.u16();
                b.pixFmt = r.u8();
            } else if constexpr (std::is_same_v<T, Frame>) {
                b.flags = headerFlags;
                b.frameIndex = r.u32();
                b.ptsMicros = r.u64();
                b.width = r.u16();
                b.height = r.u16();
                uint16_t count = r.u16();
                if (count == 0) {
                    throw ProtocolError("FRAME rect_count must be at least 1");
                }
                // Check before allocating, not after: a 26-byte message must not be able to make a
                // 65535-element vector appear.
                if (static_cast<std::size_t>(count) * 8 > r.remaining()) {
                    throw ProtocolError("FRAME rect_count " + std::to_string(count) + " needs "
                                        + std::to_string(static_cast<std::size_t>(count) * 8)
                                        + " bytes, " + std::to_string(r.remaining()) + " remain");
                }
                b.rects.clear();
                b.rects.reserve(count);
                for (uint16_t i = 0; i < count; i++) {
                    b.rects.push_back(Frame::Rect{r.i16(), r.i16(), r.i16(), r.i16()});
                }
                auto raw = r.bytes_field();
                b.data.assign(raw.begin(), raw.end());
                if (b.data.empty()) {
                    throw ProtocolError("FRAME picture must not be empty");
                }
            } else if constexpr (std::is_same_v<T, KeyframeRequest>) {
                // empty payload
            } else if constexpr (std::is_same_v<T, InputBatch>) {
                uint16_t count = r.u16();
                if (count == 0) {
                    throw ProtocolError("INPUT_BATCH count must be at least 1");
                }
                if (static_cast<std::size_t>(count) * kInputRecordSize > r.remaining()) {
                    throw ProtocolError("INPUT_BATCH count " + std::to_string(count) + " needs "
                                        + std::to_string(static_cast<std::size_t>(count)
                                                         * kInputRecordSize)
                                        + " bytes, " + std::to_string(r.remaining()) + " remain");
                }
                b.records.clear();
                b.records.reserve(count);
                for (uint16_t i = 0; i < count; i++) {
                    InputBatch::Record rec;
                    rec.kind = r.u8();
                    rec.flags = r.u8();
                    uint16_t reserved = r.u16();
                    if (reserved != 0) {
                        throw ProtocolError("input record reserved must be 0, got "
                                            + std::to_string(reserved));
                    }
                    rec.x = r.i32();
                    rec.y = r.i32();
                    rec.data = r.u32();
                    b.records.push_back(rec);
                }
            } else if constexpr (std::is_same_v<T, AgentHello>) {
                b.machineId = r.string();
                b.label = r.string();
                b.os = r.u8();
                b.capabilities = r.u16();
                b.maxWidth = r.u16();
                b.maxHeight = r.u16();
                b.encoders = r.u8();
            } else if constexpr (std::is_same_v<T, AgentHeartbeat>) {
                b.uptimeSeconds = r.u32();
                b.activeSessions = r.u16();
            } else if constexpr (std::is_same_v<T, AgentStats>) {
                b.framesSent = r.u32();
                b.framesDropped = r.u32();
                b.bytesSent = r.u64();
                b.encodeMsAvg = r.u16();
                b.bitrateKbps = r.u16();
                b.fps = r.u8();
                b.inputQueueDepth = r.u8();
            }
        },
        m.body);

    r.require_exhausted();
    return m;
}

std::vector<uint8_t> encode_message(const Message& message) {
    if (type_of(message.body) != message.type) {
        throw std::invalid_argument("message type does not match its body");
    }
    WireWriter payload;
    encode_body(payload, message);
    WireWriter out;
    out.u8(static_cast<uint8_t>(message.type));
    out.u8(body_flags(message.body));
    out.u16(0);
    out.u32(static_cast<uint32_t>(payload.size()));
    out.raw(payload.data());
    return std::vector<uint8_t>(out.data().begin(), out.data().end());
}

std::optional<int64_t> read_scalar(const Message& m, const std::string& key) {
    std::string cls;
    std::string field = key;
    if (auto dot = key.find('.'); dot != std::string::npos) {
        cls = key.substr(0, dot);
        field = key.substr(dot + 1);
    }
    // The class prefix, when present, must name the message that was actually decoded -- so a vector
    // cannot quietly assert a field of a different message.
    if (!cls.empty() && cls != type_name(m.type)) {
        return std::nullopt;
    }

    std::optional<int64_t> out;
    std::visit(
        [&out, &field](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            auto set = [&out, &field](const char* name, int64_t v) {
                if (field == name) out = v;
            };
            if constexpr (std::is_same_v<T, Hello>) {
                set("protocolMin", b.protocolMin);
                set("protocolMax", b.protocolMax);
                set("role", b.role);
            } else if constexpr (std::is_same_v<T, HelloAck>) {
                set("protocol", b.protocol);
                set("maxMessage", b.maxMessage);
                set("maxWidth", b.maxWidth);
                set("maxHeight", b.maxHeight);
            } else if constexpr (std::is_same_v<T, ErrorMsg>) {
                set("code", b.code);
            } else if constexpr (std::is_same_v<T, OpenSession>) {
                set("width", b.width);
                set("height", b.height);
                set("quality", b.quality);
            } else if constexpr (std::is_same_v<T, SessionOpened>) {
                set("width", b.width);
                set("height", b.height);
                set("capture", b.capture);
            } else if constexpr (std::is_same_v<T, CloseSession>) {
                set("reason", b.reason);
            } else if constexpr (std::is_same_v<T, Ping>) {
                set("id", b.id);
                set("micros", static_cast<int64_t>(b.micros));
            } else if constexpr (std::is_same_v<T, Pong>) {
                set("id", b.id);
                set("micros", static_cast<int64_t>(b.micros));
            } else if constexpr (std::is_same_v<T, Config>) {
                set("width", b.width);
                set("height", b.height);
                set("bitrateKbps", b.bitrateKbps);
                set("fps", b.fps);
                set("keyframeInterval", b.keyframeInterval);
            } else if constexpr (std::is_same_v<T, Frame>) {
                set("frameIndex", b.frameIndex);
                set("ptsMicros", static_cast<int64_t>(b.ptsMicros));
                set("width", b.width);
                set("height", b.height);
                set("flags", b.flags);
            } else if constexpr (std::is_same_v<T, InputBatch>) {
                set("count", static_cast<int64_t>(b.records.size()));
            } else if constexpr (std::is_same_v<T, AgentHello>) {
                set("os", b.os);
                set("capabilities", b.capabilities);
                set("maxWidth", b.maxWidth);
                set("maxHeight", b.maxHeight);
                set("encoders", b.encoders);
            } else if constexpr (std::is_same_v<T, AgentHeartbeat>) {
                set("uptimeSeconds", b.uptimeSeconds);
                set("activeSessions", b.activeSessions);
            } else if constexpr (std::is_same_v<T, AgentStats>) {
                set("framesSent", b.framesSent);
                set("framesDropped", b.framesDropped);
                set("bytesSent", static_cast<int64_t>(b.bytesSent));
                set("bitrateKbps", b.bitrateKbps);
                set("fps", b.fps);
            } else if constexpr (std::is_same_v<T, BytePayload> || std::is_same_v<T, KeyframeRequest>) {
                // no scalars; an unknown key stays nullopt
            }
        },
        m.body);
    return out;
}

}  // namespace rw
