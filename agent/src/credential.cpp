#include "rw/credential.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#ifndef _WIN32
#include <sys/stat.h>
#endif

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

bool parse_key(const std::string& text, std::vector<uint8_t>* out) {
    // The prefix is optional on the way in, so a key read from a file, a key typed by a user and a
    // key built by a test all decode the same way.
    return parse_token(text.rfind("rw1_", 0) == 0 ? text.substr(4) : text, out);
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

std::string machine_id(std::span<const uint8_t> key) {
    // FNV-1a, 64-bit, widened by a second pass with a different offset so the low half is not a
    // trivial function of the high half. 16 hex characters, stable for the life of the key.
    auto fnv = [](uint64_t hash, uint8_t b) {
        hash ^= b;
        hash *= 1099511628211ull;
        return hash;
    };
    uint64_t high = 1469598103934665603ull;
    uint64_t low = 1099511628211ull;
    for (uint8_t b : key) {
        high = fnv(high, b);
        low = fnv(low, static_cast<uint8_t>(b + 0x9E));
    }
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 8; i++) {
        out[i] = digits[(high >> ((7 - i) * 4)) & 0xF];
        out[8 + i] = digits[(low >> ((7 - i) * 4)) & 0xF];
    }
    return out;
}

std::string KeyStore::default_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata != nullptr && appdata[0] != '\0') {
        return std::string(appdata) + "\\remote-worker\\agent.key";
    }
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.config/remote-worker/agent.key";
    }
#endif
    return "remote-worker-agent.key";
}

KeyStore::KeyStore() : path_(default_path()) {}
KeyStore::KeyStore(std::string path) : path_(std::move(path)) {}

bool KeyStore::load(std::string* key_text, std::string* error) const {
    std::FILE* file = std::fopen(path_.c_str(), "rb");
    if (file == nullptr) {
        return false;  // no key yet; that is a normal first run, not an error
    }
    char buffer[256] = {0};
    std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    std::string text(buffer, got);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.rfind("rw1_", 0) != 0) {
        // Refuse rather than regenerate: overwriting a key you cannot parse would destroy it and
        // unpair the machine for no reason.
        if (error != nullptr) {
            *error = path_ + " does not start with rw1_ -- refusing to treat it as a key";
        }
        return false;
    }
    std::vector<uint8_t> parsed;
    if (!parse_key(text, &parsed) || parsed.size() != kTokenBytes) {
        if (error != nullptr) {
            *error = path_ + " is not a well-formed " + std::to_string(kTokenBytes) + "-byte key";
        }
        return false;
    }
    *key_text = text;
    return true;
}

bool KeyStore::store(const std::string& key_text, std::string* error) const {
    std::vector<uint8_t> parsed;
    if (!parse_key(key_text, &parsed) || parsed.size() != kTokenBytes) {
        if (error != nullptr) {
            *error = "refusing to store a malformed key";
        }
        return false;
    }
    // create_directories, not a shell `mkdir`: `mkdir` is a cmd builtin on Windows, so the
    // system() call only ever worked on a path whose directory already happened to exist -- the
    // first real run on a clean machine would have found no key file and no error explaining why.
    // It also creates intermediate directories, and takes an error_code rather than being able to
    // take the process down.
    std::filesystem::path path_of_file(path_);
    if (path_of_file.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(path_of_file.parent_path(), ignored);
    }
    std::FILE* file = std::fopen(path_.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = "cannot write " + path_;
        }
        return false;
    }
    std::fwrite(key_text.data(), 1, key_text.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
#ifndef _WIN32
    // Owner-only where the platform has such a thing. On Windows the file inherits the directory
    // ACL, which is the normal protection there and is not something to fake with a mode bit.
    //
    // ::chmod, not std::chmod. <sys/stat.h> declares the POSIX spelling, and libstdc++ does not
    // provide the std:: one -- GCC's own diagnostic is "did you mean 'chmod'?". This never showed
    // up locally because the whole call is inside #ifndef _WIN32 and the local toolchain is
    # MSVC, which does not compile this branch at all.
    ::chmod(path_.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

}  // namespace rw
