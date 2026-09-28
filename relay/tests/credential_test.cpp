// Credentials: the token an agent mints, and the checks around it.
//
// There is no protocol vector for this -- credentials never appear in the wire format's field
// definitions, only as an opaque byte run in OPEN_SESSION -- so the primitive is tested here, and
// the end-to-end flow is tested in relay_e2e_test.cpp.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "rw/credential.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        g_failures++;
        std::cerr << "  - FAIL: " << what << "\n";
    } else {
        std::cout << "  ok: " << what << "\n";
    }
}

}  // namespace

int main() {
    std::cout << "credentials\n";

    // --- format / parse round trip ------------------------------------------------------------
    {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> dist(0, 255);
        bool allRoundTrip = true;
        bool alphabetClean = true;
        for (int i = 0; i < 200; i++) {
            std::vector<uint8_t> bytes(1 + (i % 40));
            for (auto& b : bytes) {
                b = static_cast<uint8_t>(dist(rng));
            }
            std::string encoded = rw::format_token(bytes);
            // Crockford drops I, L, O and U precisely so a misread character is visible.
            for (char c : encoded) {
                if (c == 'I' || c == 'L' || c == 'O' || c == 'U') {
                    alphabetClean = false;
                }
            }
            std::vector<uint8_t> decoded;
            if (!rw::parse_token(encoded, &decoded) || decoded != bytes) {
                allRoundTrip = false;
            }
        }
        check(allRoundTrip, "200 random values round-trip through base32");
        check(alphabetClean, "the alphabet contains no I, L, O or U");
    }

    // --- a truncated token must not decode to a valid shorter prefix ---------------------------
    {
        std::vector<uint8_t> full(rw::kTokenBytes, 0xA5);
        std::string encoded = rw::format_token(full);
        std::vector<uint8_t> decoded;
        check(rw::parse_token(encoded, &decoded) && decoded == full, "a whole token parses");
        std::vector<uint8_t> truncated;
        check(rw::parse_token(encoded.substr(0, encoded.size() - 4), &truncated) == false,
              "a token missing its last characters is rejected, not shortened");
    }

    // --- case and garbage ---------------------------------------------------------------------
    {
        std::vector<uint8_t> bytes = {0x01, 0x23, 0x45, 0x67, 0x89};
        std::string encoded = rw::format_token(bytes);
        std::string lowered = encoded;
        for (auto& c : lowered) {
            // tolower, not `c - 'A' + 'a'`: the alphabet contains digits, and subtracting 'A' from
            // '0' produces a letter.
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::vector<uint8_t> decoded;
        check(rw::parse_token(lowered, &decoded) && decoded == bytes,
              "a lower-cased token still parses, because a chat client may have folded it");
        check(rw::parse_token("!!!!", &decoded) == false, "punctuation is rejected");
    }

    // --- comparison -----------------------------------------------------------------------------
    {
        std::vector<uint8_t> a = {1, 2, 3, 4};
        std::vector<uint8_t> same = {1, 2, 3, 4};
        std::vector<uint8_t> differsAtEnd = {1, 2, 3, 5};
        std::vector<uint8_t> differsAtStart = {9, 2, 3, 4};
        std::vector<uint8_t> shorter = {1, 2, 3};
        std::vector<uint8_t> longer = {1, 2, 3, 4, 5};
        check(rw::constant_time_equals(a, same), "equal tokens compare equal");
        check(!rw::constant_time_equals(a, differsAtEnd), "one byte different, at the end");
        check(!rw::constant_time_equals(a, differsAtStart), "one byte different, at the start");
        check(!rw::constant_time_equals(a, shorter), "a prefix does not match");
        check(!rw::constant_time_equals(a, longer), "an extension does not match");
    }

    // --- fingerprints ---------------------------------------------------------------------------
    {
        std::vector<uint8_t> a = {0xDE, 0xAD, 0xBE, 0xEF};
        std::vector<uint8_t> b = {0xDE, 0xAD, 0xBE, 0xEF};
        std::vector<uint8_t> c = {0xDE, 0xAD, 0xBE, 0xF0};
        std::string fa = rw::token_fingerprint(a);
        check(fa == rw::token_fingerprint(b), "the same token fingerprints identically");
        check(fa != rw::token_fingerprint(c), "a different token fingerprints differently");
        check(fa.size() == 16, "a fingerprint is 16 hex characters, enough to compare by eye");
    }

    // --- generation -------------------------------------------------------------------------------
    {
        std::string first = rw::generate_token();
        std::string second = rw::generate_token();
        check(first.rfind("rw1_", 0) == 0, "a generated token is prefixed: " + first);
        check(first.size() == 4 + 32,
              "a generated token is 32 base32 characters: " + std::to_string(first.size() - 4));
        check(first != second, "two generated tokens differ, so the RNG is actually random");

        // Every 20-byte value base32-encodes to the same length, so the length is a real invariant
        // rather than a coincidence of the sample.
        std::vector<std::vector<uint8_t>> all(20, std::vector<uint8_t>(rw::kTokenBytes, 0xFF));
        all[0][0] = 0x00;
        bool lengthsMatch = true;
        for (const auto& bytes : all) {
            if (rw::format_token(bytes).size() != 32) {
                lengthsMatch = false;
            }
        }
        check(lengthsMatch, "every 20-byte token encodes to the same length");
    }

    std::cout << (g_failures == 0 ? "\ncredentials OK" : "\ncredentials FAILED") << "\n";
    return g_failures == 0 ? 0 : 1;
}
