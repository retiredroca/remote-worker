// Endpoint credentials: the token an agent mints to say "this controller may watch me".
//
// The agent generates a token, shows it once, and remembers it. The mod holds the same token and
// presents it on OPEN_SESSION. The *agent* is what checks it -- the relay never sees a key, so a
// compromised relay cannot mint one for itself. See PROTOCOL.md 3.
//
// Symmetric, deliberately. A keypair (the agent holding the private half, the mod showing the public
// one) would let a controller be added without ever showing a secret, but Ed25519 verification is
// real cryptography and this project has deliberately taken no crypto dependency. A random secret
// compared in constant time gets the same property that matters here: an unpaired mod is refused.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rw {

// Raw entropy in a token. 20 bytes = 160 bits, which is well past anything guessable, and 32
// base32 characters -- short enough to read aloud or paste into a chat.
inline constexpr std::size_t kTokenBytes = 20;

// Crockford base32: the digits and letters, minus I, L, O and U so a misread character is
// detectable by eye. Uppercase, because these are shown on a terminal and a terminal's own font
// renders lowercase l and 1 confusingly.
std::string format_token(std::span<const uint8_t> bytes);
bool parse_token(const std::string& text, std::vector<uint8_t>* out);

// Decodes a key in either of its two written forms: with the "rw1_" prefix, or as the bare 32
// base32 characters. One entry point, because the prefix stripping used to be done by hand at each
// call site and had already drifted in one of them.
//
// This matters more than tidiness. The machine id is derived from the key, so hashing the key's
// *text* rather than its *bytes* gives a different id for the same key. A caller that did that would
// compute an id the agent never agrees with, and the symptom would be a machine that cannot be
// paired while every unit test still passed -- because a test that hashed the text on both sides
// agrees with itself.
bool parse_key(const std::string& text, std::vector<uint8_t>* out);

// A fresh token, formatted and ready to show: "rw1_" + 32 base32 characters.
std::string generate_token();

// Compares in time proportional to the input, not to the first difference. A byte-at-a-time early
// return leaks how much of a guessed token was correct, which is the one thing an attacker with a
// wrong token can otherwise iterate on.
bool constant_time_equals(std::span<const uint8_t> a, std::span<const uint8_t> b);

// The machine id derived from a key: a public, stable identifier peers can address a machine by.
//
// The key is the machine's identity, so the id has to be *derived* from it rather than equal to it.
// If they were the same bytes, anyone who saw a machine id -- which is not secret, it travels in
// AGENT_HELLO and in every OPEN_SESSION -- could present it as that machine's credential. Deriving
// keeps the id public and the key private, and it means regenerating the key necessarily produces a
// new identity: after a rotation nothing can prove the machine is the same one, so peers are told it
// is not.
//
// FNV-1a is not a cryptographic hash, and that is a deliberate, bounded choice here. The inputs are
// 160 bits of uniform randomness from the system CSPRNG, over which FNV behaves like a random
// function -- its weakness is with structured or adversarial inputs, not uniform ones. What the
// 64-bit output buys is that two machines colliding is astronomically unlikely at any realistic
// fleet size (about 3e-14 at a thousand machines), and if it did happen the relay refuses the second
// registration with DuplicateEndpoint, so a collision denies a machine rather than merging two
// silently. The right upgrade is a real hash, which needs the crypto dependency this project has
// deliberately not taken.
std::string machine_id(std::span<const uint8_t> key);
inline std::string machine_id(const std::string& key_text) {
    // Always from the decoded bytes, so a machine has exactly one id however the key was obtained.
    std::vector<uint8_t> bytes;
    if (!parse_key(key_text, &bytes)) {
        return std::string();
    }
    return machine_id(bytes);
}

// Where an agent keeps its key. Not a secret store: the file holds the key in the clear, which is
// the deliberate simplification recorded in PROTOCOL.md 3, and it is written owner-only.
class KeyStore {
public:
    KeyStore();
    // An explicit path, for the tests and for an agent configured somewhere other than the default.
    explicit KeyStore(std::string path);


    // Reads the key, or returns false if there is none. A malformed file is an error rather than a
    // silent "no key", because regenerating over a key you cannot parse loses it for no reason.
    bool load(std::string* key_text, std::string* error) const;

    // Creates the file with owner-only permissions where the platform has them (POSIX 0600;
    // Windows inherits the directory ACL, which is the usual protection there).
    bool store(const std::string& key_text, std::string* error) const;

    static std::string default_path();

private:
    std::string path_;
};

}  // namespace rw
