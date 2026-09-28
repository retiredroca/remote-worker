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

// A fresh token, formatted and ready to show: "rw1_" + 32 base32 characters.
std::string generate_token();

// Compares in time proportional to the input, not to the first difference. A byte-at-a-time early
// return leaks how much of a guessed token was correct, which is the one thing an attacker with a
// wrong token can otherwise iterate on.
bool constant_time_equals(std::span<const uint8_t> a, std::span<const uint8_t> b);

// A short, non-reversible identifier for a token, for logs and for "which key is this" without
// printing the secret. FNV-1a over the raw bytes: this is for labelling, never for security, and the
// distinction matters -- it is not a hash you may use to verify anything.
std::string token_fingerprint(std::span<const uint8_t> bytes);

}  // namespace rw
