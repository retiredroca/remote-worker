// Framing over a byte stream, and the two stream implementations: a plain socket and (later) TLS.
//
// The seam exists so the agent's session logic never knows whether it is moving plaintext or
// ciphertext, and so the tests can drive the real code path over loopback.
//
// --- where transport security plugs in -----------------------------------------------------------
// Transport security is deferred, not rejected (PROTOCOL.md 3). When it lands it is one new class
// beside SocketStream, implementing ByteStream over a TLS session:
//
//     class TlsStream : public ByteStream { ... };   // handshake, then read/write on the session
//
// and the agent wraps the accepted socket instead of adopting it. Nothing above this line changes:
//   - The agent never reads or writes bytes itself. All I/O goes through the peer's channel, which
//     is a FramedChannel over a ByteStream. The only socket it touches directly is the listener,
//     for accept().
//   - FramedChannel knows nothing about TLS, so record boundaries survive a partial read either way.
//   - The accept path is the single place a TlsStream would be constructed.
//
// The agent binds every interface by design, because it authenticates every request. With TLS in
// place the open question it answers is who is on the other end, not whether to listen.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rw/protocol.hpp"

namespace rw {

// A bidirectional byte pipe. read() returns 0 on clean end of stream.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual std::size_t read(uint8_t* buf, std::size_t len) = 0;
    virtual void write_all(const uint8_t* buf, std::size_t len) = 0;

    void write_all(std::span<const uint8_t> bytes) { write_all(bytes.data(), bytes.size()); }
};

// A message as it arrived, with the exact bytes it arrived in.
//
// Both halves are kept because a message is decoded to decide what to do, and re-encoding a decoded
// video frame would be a second full copy of every frame on the hot path, for no benefit.
struct ReceivedFrame {
    std::vector<uint8_t> raw;
    Message message;
};

// Length-prefixed message framing on top of a byte stream.
class FramedChannel {
public:
    explicit FramedChannel(ByteStream& stream) : stream_(stream) {}

    // Reads exactly one message. std::nullopt means the peer closed cleanly on a message boundary;
    // a truncated message throws ProtocolError, because a partial message is a fault, not a close.
    std::optional<ReceivedFrame> read_frame();

    // Frames and writes one message.
    void write_message(const Message& message);

    // Writes an already-framed message, for forwarding without a decode/encode round trip.
    void write_raw(std::span<const uint8_t> framed_message);

private:
    ByteStream& stream_;
};

// Process-wide socket setup, so a Windows binary does not have to remember WSAStartup. Idempotent.
class SocketRuntime {
public:
    static void ensure_initialised();
};

// A non-blocking TCP socket. Owns the handle and closes it.
class SocketStream : public ByteStream {
public:
    SocketStream() = default;
    explicit SocketStream(int fd) : fd_(fd) {}
    ~SocketStream() override;

    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;
    SocketStream(SocketStream&& other) noexcept;
    SocketStream& operator=(SocketStream&& other) noexcept;

    static std::optional<SocketStream> connect_loopback(uint16_t port);
    // Binds `address` (dotted-quad or a name the resolver understands) on `port`, or on an
    // OS-chosen port when `port` is 0. The port actually bound is reported through `bound_port`.
    // Passing 0.0.0.0 really does bind every interface, so what the daemon logs is what it did.
    static std::optional<SocketStream> listen(const std::string& address, uint16_t port,
                                             uint16_t* bound_port);

    // Non-blocking. POLLIN is signalled when a message's worth of bytes may be available;
    // has_message_available() does the read_exact loop that decides that.
    bool has_message_available();
    std::size_t read(uint8_t* buf, std::size_t len) override;
    void write_all(const uint8_t* buf, std::size_t len) override;

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void close();
    void set_nonblocking();

private:
    int fd_ = -1;
};

}  // namespace rw
