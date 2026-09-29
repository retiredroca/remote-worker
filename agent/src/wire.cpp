#include "rw/wire.hpp"

namespace rw {

void WireWriter::ensure(std::size_t extra) {
    // Never grow past what a conforming message may occupy: an unbounded buffer here would
    // reintroduce, on the encode side, the allocation attack kMaxMessage exists to stop.
    if (buf_.size() + extra > kMaxMessage) {
        throw std::invalid_argument("message exceeds MAX_MESSAGE at "
                                    + std::to_string(buf_.size() + extra) + " bytes");
    }
    buf_.reserve(buf_.size() + extra);
}

WireWriter& WireWriter::u8(uint8_t v) {
    ensure(1);
    buf_.push_back(v);
    return *this;
}

WireWriter& WireWriter::u16(uint16_t v) {
    ensure(2);
    buf_.push_back(static_cast<uint8_t>(v & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    return *this;
}

WireWriter& WireWriter::u32(uint32_t v) {
    ensure(4);
    for (int i = 0; i < 4; i++) {
        buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    return *this;
}

WireWriter& WireWriter::u64(uint64_t v) {
    ensure(8);
    for (int i = 0; i < 8; i++) {
        buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    return *this;
}

WireWriter& WireWriter::i16(int16_t v) {
    return u16(static_cast<uint16_t>(v));
}

WireWriter& WireWriter::i32(int32_t v) {
    return u32(static_cast<uint32_t>(v));
}

WireWriter& WireWriter::raw(std::span<const uint8_t> bytes) {
    ensure(bytes.size());
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    return *this;
}

WireWriter& WireWriter::string(std::string_view s) {
    if (!is_valid_utf8(s)) {
        throw std::invalid_argument("string is not valid UTF-8; a decoder would have to refuse it");
    }
    if (s.size() > kMaxString) {
        throw std::invalid_argument("string of " + std::to_string(s.size())
                                    + " bytes exceeds MAX_STRING");
    }
    u16(static_cast<uint16_t>(s.size()));
    return raw(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

WireWriter& WireWriter::bytes_field(std::span<const uint8_t> bytes) {
    u32(static_cast<uint32_t>(bytes.size()));
    return raw(bytes);
}

void WireReader::require(std::size_t n) const {
    if (n > remaining()) {
        throw ProtocolError("read of " + std::to_string(n) + " exceeds "
                            + std::to_string(remaining()) + " remaining");
    }
}

uint8_t WireReader::u8() {
    require(1);
    return buf_[pos_++];
}

uint16_t WireReader::u16() {
    require(2);
    uint16_t v = static_cast<uint16_t>(buf_[pos_]) |
                 static_cast<uint16_t>(static_cast<uint16_t>(buf_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

uint32_t WireReader::u32() {
    require(4);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= static_cast<uint32_t>(buf_[pos_ + i]) << (8 * i);
    }
    pos_ += 4;
    return v;
}

uint64_t WireReader::u64() {
    require(8);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= static_cast<uint64_t>(buf_[pos_ + i]) << (8 * i);
    }
    pos_ += 8;
    return v;
}

int16_t WireReader::i16() {
    return static_cast<int16_t>(u16());
}

int32_t WireReader::i32() {
    return static_cast<int32_t>(u32());
}

std::span<const uint8_t> WireReader::bytes(std::size_t n) {
    require(n);
    auto out = buf_.subspan(pos_, n);
    pos_ += n;
    return out;
}

std::span<const uint8_t> WireReader::bytes_field() {
    uint32_t n = u32();
    if (n > remaining()) {
        throw ProtocolError("byte run of " + std::to_string(n) + " exceeds "
                            + std::to_string(remaining()) + " remaining");
    }
    return bytes(n);
}

std::string WireReader::string() {
    uint16_t n = u16();
    if (n > kMaxString) {
        throw ProtocolError("string length " + std::to_string(n) + " exceeds MAX_STRING");
    }
    auto raw_bytes = bytes(n);
    if (!is_valid_utf8(raw_bytes)) {
        throw ProtocolError("string is not valid UTF-8");
    }
    return std::string(reinterpret_cast<const char*>(raw_bytes.data()), raw_bytes.size());
}

void WireReader::require_exhausted() {
    if (remaining() != 0) {
        throw ProtocolError(std::to_string(remaining()) + " trailing byte(s) after message");
    }
}

bool is_valid_utf8(std::span<const uint8_t> bytes) {
    std::size_t i = 0;
    const std::size_t n = bytes.size();
    while (i < n) {
        uint8_t b0 = bytes[i];
        if (b0 < 0x80) {
            i++;
            continue;
        }
        int extra;
        uint32_t cp;
        if ((b0 & 0xE0) == 0xC0) {
            extra = 1;
            cp = b0 & 0x1Fu;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra = 2;
            cp = b0 & 0x0Fu;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra = 3;
            cp = b0 & 0x07u;
        } else {
            return false;  // continuation byte with no lead, or 5/6-byte form
        }
        // The sequence needs `extra` continuation bytes after position i, so it needs i+extra < n.
        if (i + static_cast<std::size_t>(extra) >= n) {
            return false;  // truncated at the end of the input
        }
        for (int k = 1; k <= extra; k++) {
            uint8_t bk = bytes[i + k];
            if ((bk & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        // Overlong forms, surrogate halves, and anything past the last code point are all invalid
        // even though they decode to *some* number; accepting them is how two peers end up
        // disagreeing about a string's identity.
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

}  // namespace rw
