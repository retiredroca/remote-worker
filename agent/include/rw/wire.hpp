// Little-endian wire primitives, field for field the same as the Writer/Reader in
// tools/protocol_vectors.py and the WireWriter/WireReader in the mod's common sources. See
// PROTOCOL.md.
//
// Field names in protocol.hpp are camelCase on purpose: they are the same words the Java codec and
// the vector expectations use, so a field means one thing in all three languages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rw {

// Longest string, in UTF-8 bytes. The protocol carries ids and diagnostics, not file contents.
inline constexpr std::size_t kMaxString = 1024;

// Largest payload a peer may announce. Bounds the allocation a single length field can provoke; see
// PROTOCOL.md 2.3.
inline constexpr std::size_t kMaxMessage = 16u * 1024u * 1024u;

// Thrown for anything a peer can cause: a length that runs past the buffer, a reserved field that
// is not zero, a string that is not valid UTF-8. The connection boundary is the only place that
// decides whether to drop the peer, so nothing below it catches this.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Growable little-endian byte sink. Out-of-range values throw std::invalid_argument rather than
// truncating: those are programmer errors, not wire errors, and belong at the call that made them.
class WireWriter {
public:
    // Each returns *this so a message body reads as one expression, in the same order the
    // normative encoder writes it.
    WireWriter& u8(uint8_t v);
    WireWriter& u16(uint16_t v);
    WireWriter& u32(uint32_t v);
    WireWriter& u64(uint64_t v);
    WireWriter& i16(int16_t v);
    WireWriter& i32(int32_t v);
    WireWriter& raw(std::span<const uint8_t> bytes);

    // Length-prefixed UTF-8, length in bytes. Not NUL-terminated: a terminator and a length would
    // be two answers to "how long is this", and they disagree whenever the payload contains a NUL.
    //
    // The field is validated rather than transcoded, and the bytes are stored as given. That keeps
    // a round trip byte-exact by construction, and stops an encoder from emitting something a
    // decoder is required to reject.
    WireWriter& string(std::string_view s);
    WireWriter& bytes_field(std::span<const uint8_t> bytes);

    std::span<const uint8_t> data() const { return {buf_.data(), buf_.size()}; }
    std::size_t size() const { return buf_.size(); }

private:
    void ensure(std::size_t extra);

    std::vector<uint8_t> buf_;
};

// Bounds-checked little-endian reader over a fixed region. Every read checks first and throws, so
// no field read can walk off the end of a message: those length fields are chosen by the peer.
class WireReader {
public:
    explicit WireReader(std::span<const uint8_t> region) : buf_(region) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int16_t i16();
    int32_t i32();
    std::span<const uint8_t> bytes(std::size_t n);
    std::span<const uint8_t> bytes_field();

    // Strict UTF-8: the bytes are validated and handed back unchanged. Malformed input is refused
    // rather than replaced, because a replacement would re-encode to different bytes than arrived
    // and the two peers would disagree about the id with nothing reporting an error.
    std::string string();

    std::size_t remaining() const { return buf_.size() - pos_; }

    // Refuses trailing bytes. See Protocol::parse_message.
    void require_exhausted();

private:
    void require(std::size_t n) const;

    std::span<const uint8_t> buf_;
    std::size_t pos_ = 0;
};

// Rejects overlong encodings, surrogate halves, and anything above U+10FFFF. Not cryptography, so
// hand-rolling is fine here -- but it is exercised by the vectors: a lone 0x80 continuation byte
// must be refused, and a Cyrillic id must survive a round trip byte for byte.
bool is_valid_utf8(std::span<const uint8_t> bytes);
inline bool is_valid_utf8(std::string_view s) {
    return is_valid_utf8(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

}  // namespace rw
