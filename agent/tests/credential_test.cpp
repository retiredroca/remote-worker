// Credentials: the token an agent mints, and the checks around it.
//
// There is no protocol vector for this -- credentials never appear in the wire format's field
// definitions, only as an opaque byte run in OPEN_SESSION -- so the primitive is tested here, and
// the end-to-end flow is tested in agent_test.cpp.
#include <algorithm>
#include <cctype>
#include <cstdio>
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

    // --- the machine id, which is now the identity ------------------------------------------------
    {
        std::vector<uint8_t> keyA = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};
        std::vector<uint8_t> keyB = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x34};
        std::vector<uint8_t> keyAagain = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};

        check(rw::machine_id(keyA) == rw::machine_id(keyAagain),
              "the same key always yields the same machine id, so a machine keeps its identity");
        check(rw::machine_id(keyA) != rw::machine_id(keyB),
              "a different key yields a different machine id, so rotating the key changes identity");
        check(rw::machine_id(keyA).size() == 16,
              "a machine id is 16 hex characters: " + rw::machine_id(keyA));

        // The invariant that lets a caller hold a key either way round. The id is derived from the
        // key, so if one caller derived it from the key's text and another from its decoded bytes,
        // the two would disagree about a machine's identity -- and a test that used the same form on
        // both sides would still pass. This pins the two forms to one answer.
        {
            // format_token is the bare base32; "rw1_" is what a user is shown, so both matter.
            const std::string keyAText = "rw1_" + rw::format_token(keyA);
            std::vector<uint8_t> fromText;
            check(rw::parse_key(keyAText, &fromText), "a key with its rw1_ prefix decodes");
            check(rw::parse_key(keyAText.substr(4), &fromText),
                  "and so does the same key without it");
            check(fromText == keyA, "and both forms decode to the same bytes");
            check(rw::machine_id(keyA) == rw::machine_id(fromText),
                  "a machine has one id whether the key is held as text or as bytes");
            check(rw::machine_id(keyA) == rw::machine_id("rw1_" + rw::format_token(fromText)),
                  "and re-encoding and decoding a key does not change its identity");
            std::vector<uint8_t> wrong;
            check(rw::parse_key("rw1_not-a-key", &wrong) == false,
                  "a malformed key is refused rather than half-decoded");
        }

        // The id travels in AGENT_HELLO and in every OPEN_SESSION, so it must not be usable as the
        // credential. If id and key were the same bytes, anyone who read an id off the wire could
        // present it as that machine's key. The real statement of that: offer the id as a key and
        // it must not match.
        std::vector<uint8_t> realKey(rw::kTokenBytes);
        for (std::size_t i = 0; i < realKey.size(); i++) {
            realKey[i] = static_cast<uint8_t>(i * 7 + 3);
        }
        std::string realId = rw::machine_id(realKey);
        std::vector<uint8_t> idOfferedAsKey;
        rw::parse_token(realId, &idOfferedAsKey);
        check(!rw::constant_time_equals(realKey, idOfferedAsKey),
              "the machine id, offered as a key, does not match the real one");
        check(realId != rw::format_token(realKey),
              "and the id is not simply the key encoded");

        // The realistic case: two real tokens, and the ids must not collide.
        std::vector<std::string> ids;
        std::vector<std::string> keys;
        for (int i = 0; i < 2000; i++) {
            std::string k = rw::generate_token();
            std::vector<uint8_t> raw;
            if (!rw::parse_token(k.substr(4), &raw)) continue;
            keys.push_back(k);
            ids.push_back(rw::machine_id(raw));
        }
        std::vector<std::string> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
              "2000 generated machines all get distinct ids");

        // Regenerating a key must be visible as a different machine, not silently the same one.
        std::string before = rw::machine_id(keys.front());
        std::string after = rw::machine_id(keys.back());
        check(before != after, "regenerating a key produces a new identity, as it must");
    }

    // --- the key store, which is what makes regeneration real --------------------------------------
    {
        std::string path = std::string(rw::KeyStore::default_path()) + ".test";
        std::remove(path.c_str());
        rw::KeyStore store(path);

        std::string loaded;
        std::string error;
        check(!store.load(&loaded, &error), "a missing key file is a normal first run, not an error");

        std::string key = rw::generate_token();
        check(store.store(key, &error), "a key can be stored");
        check(store.load(&loaded, &error) && loaded == key, "and read back unchanged");

        // Refuse rather than regenerate: overwriting a key you cannot parse would unpair the
        // machine for no reason, and the operator would have no idea it had happened.
        {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            std::fputs("this is not a key\n", f);
            std::fclose(f);
        }
        loaded.clear();
        error.clear();
        check(!store.load(&loaded, &error) && !error.empty(),
              "a malformed key file is an error, not a silent 'no key' that invites a regen");
        check(store.load(&loaded, &error) == false && store.store("rw1_nonsense", &error) == false,
              "and a malformed key is refused rather than written over the good one");
        std::remove(path.c_str());
    }

    // --- generation -------------------------------------------------------------------------------
    {
        std::string first = rw::generate_token();
        std::string second = rw::generate_token();
        check(first.rfind("rw1_", 0) == 0, "a generated token is prefixed: " + first);
        check(first.size() == 4 + 32,
              "a generated token is 32 base32 characters: " + std::to_string(first.size() - 4));

        // Uniqueness is the property the whole credential scheme rests on: two machines each mint
        // their own, and the controller presents the right one. Two samples prove very little, so
        // take many and also check the leading bytes move -- a constant seed, or an RNG that is only
        // partly wired up, would repeat early and show up here.
        std::vector<std::string> tokens;
        std::vector<std::vector<uint8_t>> leadingBytes;
        for (int i = 0; i < 1000; i++) {
            std::string t = rw::generate_token();
            tokens.push_back(t);
            std::vector<uint8_t> raw;
            if (rw::parse_token(t.substr(4), &raw)) {
                // uint8_t, not int: a ternary yields int, and int in a braced init list for a
                // byte vector is a narrowing conversion.
                const uint8_t lead = raw.empty() ? uint8_t{0} : raw[0];
                const uint8_t next = raw.size() > 1 ? raw[1] : uint8_t{0};
                leadingBytes.push_back({lead, next});
            }
        }
        std::sort(tokens.begin(), tokens.end());
        auto firstDup = std::adjacent_find(tokens.begin(), tokens.end());
        check(firstDup == tokens.end(), "1000 generated tokens are all distinct");

        std::sort(leadingBytes.begin(), leadingBytes.end());
        leadingBytes.erase(std::unique(leadingBytes.begin(), leadingBytes.end()),
                           leadingBytes.end());
        // Not "no duplicates": 1000 draws from a 2-byte space collide ~7 times by the birthday
        // paradox, so demanding uniqueness here would be demanding an impossibility. The property
        // worth asserting is that the head is not fixed -- a constant seed, or an RNG only partly
        // wired up, would collapse this to a handful of values. ~992 of the 1000 are expected
        // distinct, so 950 is a wide margin that still catches a real fault.
        check(leadingBytes.size() > 950,
              "the leading bytes vary rather than repeating a fixed prefix (" +
                  std::to_string(leadingBytes.size()) + "/1000 distinct)");

        check(tokens[0] != tokens[999], "two generated tokens differ, so the RNG is actually random");
    }

    std::cout << (g_failures == 0 ? "\ncredentials OK" : "\ncredentials FAILED") << "\n";
    return g_failures == 0 ? 0 : 1;
}
