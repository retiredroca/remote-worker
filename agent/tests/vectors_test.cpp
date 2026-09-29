// The C++ half of the wire-format conformance check.
//
// Reads the same vectors that tools/protocol_vectors.py generates for the Java check and asserts the
// same two properties: a ROUNDTRIP message decodes and re-encodes to identical bytes, and a REJECT
// message is refused with a ProtocolError. It also verifies the pinned `expect=` scalars, because
// byte comparison is blind to a swap of two same-width fields -- see PROTOCOL.md 2.4.
//
// Every failure is reported, not just the first: a drifted format breaks several vectors at once.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rw/protocol.hpp"
#include "rw/wire.hpp"

namespace {

int g_failures = 0;
int g_roundtrips = 0;
int g_rejects = 0;

std::vector<uint8_t> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd-length hex");
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("not hex");
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s += digits[(b >> 4) & 0xF];
        s += digits[b & 0xF];
    }
    return s;
}

void fail(const std::string& name, const std::string& why) {
    g_failures++;
    std::cerr << "  - " << name << ": " << why << "\n";
}

void check_roundtrip(const std::string& name, const std::vector<uint8_t>& bytes,
                     std::string_view expect) {
    g_roundtrips++;
    rw::Message decoded;
    try {
        decoded = rw::parse_message(bytes);
    } catch (const rw::ProtocolError& e) {
        fail(name, std::string("ROUNDTRIP vector was rejected: ") + e.what());
        return;
    } catch (const std::exception& e) {
        fail(name, std::string("ROUNDTRIP vector threw the wrong type: ") + e.what());
        return;
    }

    rw::Header header = rw::parse_header(bytes);
    if (header.length != bytes.size() - rw::kHeaderSize) {
        fail(name, "header length " + std::to_string(header.length) + " does not match payload of "
                       + std::to_string(bytes.size() - rw::kHeaderSize) + " bytes");
    }
    if (header.type != decoded.type) {
        fail(name, "header type does not match the decoded body");
    }
    if (header.flags != rw::body_flags(decoded.body)) {
        // The body must carry the same flags the header did, or a consumer reading the body
        // disagrees with the wire about whether, say, a frame is a keyframe.
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%x", header.flags);
        fail(name, std::string("header flags ") + buf + " but the body implies "
                    + std::to_string(rw::body_flags(decoded.body)));
    }

    std::vector<uint8_t> reencoded = rw::encode_message(decoded);
    if (reencoded != bytes) {
        fail(name, "re-encode differs\n      expected " + to_hex(bytes)
                       + "\n      actual   " + to_hex(reencoded));
        return;
    }

    if (expect.empty()) {
        return;
    }
    // Pinned scalars, comma-separated "Class.field=value".
    size_t pos = 0;
    while (pos <= expect.size()) {
        size_t comma = expect.find(',', pos);
        std::string_view item = expect.substr(pos, comma == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : comma - pos);
        if (!item.empty()) {
            size_t eq = item.find('=');
            if (eq == std::string_view::npos) {
                fail(name, "malformed expectation '" + std::string(item) + "'");
            } else {
                std::string key(item.substr(0, eq));
                int64_t want = 0;
                try {
                    want = std::stoll(std::string(item.substr(eq + 1)));
                } catch (const std::exception&) {
                    fail(name, "expectation '" + std::string(item) + "' is not an integer");
                    want = 0;
                }
                auto actual = rw::read_scalar(decoded, key);
                if (!actual) {
                    fail(name, key + ": no such field, or it names a different message than the "
                                 "one decoded (" + rw::type_name(decoded.type) + ")");
                } else if (*actual != want) {
                    fail(name, key + " is " + std::to_string(*actual) + ", expected "
                                     + std::to_string(want)
                                     + "  -- adjacent same-width fields swapped?");
                }
            }
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
}

void check_reject(const std::string& name, const std::vector<uint8_t>& bytes) {
    g_rejects++;
    try {
        rw::Message decoded = rw::parse_message(bytes);
        fail(name, std::string("REJECT vector was accepted as ") + rw::type_name(decoded.type)
                       + " -- a decoder that reads past the end of a buffer is a remote crash, and "
                         "one that ignores a trailing length is a way to smuggle a second message");
    } catch (const rw::ProtocolError&) {
        // The point of the vector.
    } catch (const std::exception& e) {
        fail(name, std::string("rejected with the wrong exception type: ") + e.what()
                       + " -- callers catch ProtocolError, so anything else escapes the connection "
                         "boundary");
    }
}

std::vector<std::string_view> split(std::string_view line, char sep) {
    std::vector<std::string_view> parts;
    size_t pos = 0;
    while (true) {
        size_t next = line.find(sep, pos);
        if (next == std::string_view::npos) {
            parts.push_back(line.substr(pos));
            return parts;
        }
        parts.push_back(line.substr(pos, next - pos));
        pos = next + 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: vectors_test <vectors.txt>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        std::cerr << "it is generated; run: python tools/protocol_vectors.py " << argv[1] << "\n";
        return 2;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string_view trimmed(line);
        trimmed.remove_prefix(start);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto parts = split(trimmed, ' ');
        // Trailing spaces would show up as an empty part.
        while (!parts.empty() && parts.back().empty()) parts.pop_back();
        if (parts.size() < 3 || parts.size() > 4) {
            fail("<line>", "malformed vector line: " + line);
            continue;
        }
        std::string kind(parts[0]);
        std::string name(parts[1]);
        std::vector<uint8_t> bytes;
        try {
            bytes = from_hex(parts[2]);
        } catch (const std::exception& e) {
            fail(name, std::string("not valid hex: ") + e.what());
            continue;
        }
        std::string expect;
        if (parts.size() == 4) {
            std::string_view clause = parts[3];
            if (clause.rfind("expect=", 0) != 0) {
                fail(name, "expected an 'expect=' clause, got: " + std::string(clause));
                continue;
            }
            expect = std::string(clause.substr(7));
        }

        if (kind == "ROUNDTRIP") {
            check_roundtrip(name, bytes, expect);
        } else if (kind == "REJECT") {
            check_reject(name, bytes);
        } else {
            fail(name, "unknown vector kind '" + kind + "'");
        }
    }

    // An empty or truncated file would otherwise "pass" by asserting nothing.
    if (g_roundtrips == 0) g_failures++, std::cerr << "  - no ROUNDTRIP vectors found\n";
    if (g_rejects == 0) g_failures++, std::cerr << "  - no REJECT vectors found\n";

    std::cout << "protocol vectors (C++): " << g_roundtrips << " round-trip, " << g_rejects
              << " reject\n";
    if (g_failures != 0) {
        std::cerr << "\n" << g_failures << " FAILURE(S).\n";
        std::cerr << "If the format changed on purpose, update tools/protocol_vectors.py and\n"
                  << "PROTOCOL.md together, then rebuild.\n";
        return 1;
    }
    std::cout << "protocol conformance OK (C++)\n";
    return 0;
}
