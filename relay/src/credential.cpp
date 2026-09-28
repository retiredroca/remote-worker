#include "rw/credential.hpp"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
// bcrypt.h must follow windows.h, and both need NOMINMAX or the min/max macros break the std headers.
#include <bcrypt.h>
#else
#include <cerrno>
#include <cstdio>
#endif

namespace rw {
namespace {

// Crockford base32, minus I, L, O and U.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Maps a character to its value, or -1. Case-insensitive, because a token that travelled through a
// chat client may have been lower-cased, and the ambiguity-free alphabet exists to be forgiving.
int decode_char(char c) {
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    for (int i = 0; i < 32; i++) {
        if (kAlphabet[i] == c) {
            return i;
        }
    }
    return -1;
}

}  // namespace

std::string format_token(std::span<const uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() * 8 + 4) / 5);
    uint16_t buffer = 0;
    int bits = 0;
    for (uint8_t byte : bytes) {
        buffer = static_cast<uint16_t>((buffer << 8) | byte);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out += kAlphabet[(buffer >> bits) & 0x1F];
        }
    }
    if (bits > 0) {
        out += kAlphabet[(buffer << (5 - bits)) & 0x1F];
    }
    return out;
}

bool parse_token(const std::string& text, std::vector<uint8_t>* out) {
    out->clear();
    uint16_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        int value = decode_char(c);
        if (value < 0) {
            return false;
        }
        buffer = static_cast<uint16_t>((buffer << 5) | value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    // Whatever is left must be zero padding, not a partial byte. A truncated token would otherwise
    // decode to a shorter, still-valid prefix.
    if (bits >= 5 || (buffer & ((1u << bits) - 1u)) != 0) {
        out->clear();
        return false;
    }
    return true;
}

std::string generate_token() {
    std::vector<uint8_t> bytes(kTokenBytes);
#ifdef _WIN32
    // BCryptGenRandom with the system-preferred RNG. Not std::random_device: it is not required to be
    // a CSPRNG, and a predictable token makes every other check here decorative.
    NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                     BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        throw std::runtime_error("BCryptGenRandom failed: 0x" + std::to_string(status));
    }
#else
    std::FILE* urandom = std::fopen("/dev/urandom", "rb");
    if (urandom == nullptr) {
        throw std::runtime_error("cannot open /dev/urandom");
    }
    std::size_t got = std::fread(bytes.data(), 1, bytes.size(), urandom);
    std::fclose(urandom);
    if (got != bytes.size()) {
        throw std::runtime_error("short read from /dev/urandom");
    }
#endif
    return "rw1_" + format_token(bytes);
}

bool constant_time_equals(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    // Compare the full length of both, so the running time does not reveal which is longer either.
    std::size_t length = a.size() > b.size() ? a.size() : b.size();
    uint8_t difference = 0;
    for (std::size_t i = 0; i < length; i++) {
        uint8_t x = i < a.size() ? a[i] : 0;
        uint8_t y = i < b.size() ? b[i] : 0;
        difference |= static_cast<uint8_t>(x ^ y);
    }
    // A length mismatch is a mismatch, but it is checked after the loop so that path costs the same.
    return difference == 0 && a.size() == b.size();
}

std::string token_fingerprint(std::span<const uint8_t> bytes) {
    // FNV-1a, 64-bit. For labelling only -- never to verify a token.
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (int i = 15; i >= 0; i--) {
        out += digits[(hash >> (i * 4)) & 0xF];
    }
    return out;
}

}  // namespace rw
