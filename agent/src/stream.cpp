#include "rw/stream.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
// netdb.h declares addrinfo, getaddrinfo and freeaddrinfo, which listen() uses. On Windows they
// come from ws2tcpip.h instead. Nothing on POSIX includes it for you.
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace rw {
namespace {

#ifdef _WIN32
void close_handle(int fd) {
    if (fd >= 0) closesocket(static_cast<SOCKET>(fd));
}
using PollFd = WSAPOLLFD;
int poll_sockets(PollFd* fds, unsigned long n, int timeout_ms) {
    return WSAPoll(fds, n, timeout_ms);
}
constexpr short kPollIn = POLLRDNORM;
#else
void close_handle(int fd) {
    if (fd >= 0) ::close(fd);
}
using PollFd = struct pollfd;
int poll_sockets(PollFd* fds, unsigned long n, int timeout_ms) {
    return ::poll(fds, static_cast<nfds_t>(n), timeout_ms);
}
constexpr short kPollIn = POLLIN;
#endif

}  // namespace

void SocketRuntime::ensure_initialised() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    });
#endif
}

// --- FramedChannel ------------------------------------------------------------------------------

std::optional<ReceivedFrame> FramedChannel::read_frame() {
    // Header first: it carries the length, and the length is bounded before anything is allocated.
    uint8_t header[kHeaderSize];
    std::size_t got = 0;
    while (got < kHeaderSize) {
        std::size_t n = stream_.read(header + got, kHeaderSize - got);
        if (n == 0) {
            if (got == 0) {
                return std::nullopt;  // clean close on a message boundary
            }
            throw ProtocolError("peer closed mid-header after " + std::to_string(got) + " bytes");
        }
        got += n;
    }

    std::size_t length = 0;
    for (int i = 4; i < 8; i++) {
        length |= static_cast<std::size_t>(header[i]) << (8 * (i - 4));
    }
    uint16_t reserved = static_cast<uint16_t>(header[2])
                        | static_cast<uint16_t>(static_cast<uint16_t>(header[3]) << 8);
    if (reserved != 0) {
        throw ProtocolError("reserved must be 0, got " + std::to_string(reserved));
    }
    if (length > kMaxMessage) {
        throw ProtocolError("length " + std::to_string(length) + " exceeds MAX_MESSAGE");
    }

    std::vector<uint8_t> frame(kHeaderSize + length);
    std::memcpy(frame.data(), header, kHeaderSize);
    std::size_t body = 0;
    while (body < length) {
        std::size_t n = stream_.read(frame.data() + kHeaderSize + body, length - body);
        if (n == 0) {
            throw ProtocolError("peer closed mid-payload after " + std::to_string(body) + " of "
                                + std::to_string(length) + " bytes");
        }
        body += n;
    }
    Message message = parse_message(frame);
    return ReceivedFrame{std::move(frame), std::move(message)};
}

void FramedChannel::write_message(const Message& message) {
    write_raw(encode_message(message));
}

void FramedChannel::write_raw(std::span<const uint8_t> framed_message) {
    stream_.write_all(framed_message.data(), framed_message.size());
}

// --- SocketStream -------------------------------------------------------------------------------

SocketStream::~SocketStream() {
    close();
}

SocketStream::SocketStream(SocketStream&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

SocketStream& SocketStream::operator=(SocketStream&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void SocketStream::close() {
    close_handle(fd_);
    fd_ = -1;
}

void SocketStream::set_nonblocking() {
    if (fd_ < 0) return;
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(static_cast<SOCKET>(fd_), FIONBIO, &mode);
#else
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif
}

std::size_t SocketStream::read(uint8_t* buf, std::size_t len) {
    if (fd_ < 0 || len == 0) return 0;
    auto n = ::recv(fd_, reinterpret_cast<char*>(buf), static_cast<int>(len), 0);
    if (n <= 0) {
        // A non-blocking socket with nothing available is not EOF; the caller decides that from
        // the poll flags, so this reports "no progress" rather than pretending the peer hung up.
        return 0;
    }
    return static_cast<std::size_t>(n);
}

void SocketStream::write_all(const uint8_t* buf, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        auto n = ::send(fd_, reinterpret_cast<const char*>(buf + sent),
                        static_cast<int>(len - sent), 0);
        if (n <= 0) {
            throw std::runtime_error("send failed after " + std::to_string(sent) + " of "
                                     + std::to_string(len) + " bytes");
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::string SocketStream::remote_address() const {
    if (fd_ < 0) {
        return std::string();
    }
    sockaddr_in addr{};
#ifdef _WIN32
    int addrLen = sizeof(addr);
#else
    socklen_t addrLen = sizeof(addr);
#endif
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        return std::string();
    }
    char text[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text)) == nullptr) {
        return std::string();
    }
    return std::string(text);
}

bool SocketStream::has_message_available() {
    if (fd_ < 0) return false;
    PollFd pfd{};
    pfd.fd = fd_;
    pfd.events = kPollIn;
    int rc = poll_sockets(&pfd, 1, 0);
    if (rc <= 0) return false;
    // Windows reports POLLRDNORM for a readable socket and POSIX reports POLLIN; WSAPOLLFD is a
    // typedef of struct pollfd, so the field is revents on both. A partial header counts as
    // readable and read_frame() does the read_exact loop.
    return (pfd.revents & (kPollIn | POLLIN)) != 0;
}

std::optional<SocketStream> SocketStream::connect_loopback(uint16_t port) {
    SocketRuntime::ensure_initialised();
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return std::nullopt;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_handle(fd);
        return std::nullopt;
    }
    return SocketStream(fd);
}

std::optional<SocketStream> SocketStream::listen(const std::string& address, uint16_t port,
                                                 uint16_t* bound_port) {
    SocketRuntime::ensure_initialised();

    // Resolve first, so an unresolvable name is a clean failure rather than a socket that binds
    // something else. getaddrinfo, not gethostbyname: the latter is deprecated on Windows and cannot
    // express an IPv6 result.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (::getaddrinfo(address.c_str(), nullptr, &hints, &results) != 0 || results == nullptr) {
        return std::nullopt;
    }
    sockaddr_in resolved{};
    std::memcpy(&resolved, results->ai_addr, sizeof(resolved));
    ::freeaddrinfo(results);

    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return std::nullopt;
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

    sockaddr_in addr = resolved;
    addr.sin_port = htons(port);  // 0 leaves the choice to the OS, which is what the tests rely on
    // The address the caller asked for, not a hardcoded loopback. The daemon's startup line claims
    // to say where it is listening, so it had better be true -- especially for 0.0.0.0, which is
    // exactly the case the unauthenticated warning is about.
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_handle(fd);
        return std::nullopt;
    }
    if (::listen(fd, 16) != 0) {
        close_handle(fd);
        return std::nullopt;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        close_handle(fd);
        return std::nullopt;
    }
    if (bound_port != nullptr) {
        *bound_port = ntohs(addr.sin_port);
    }
    // The listener must be non-blocking. accept() on a blocking socket does not return "nothing is
    // queued" -- it waits -- and both daemons call accept() at the top of every run_once(), so a
    // blocking listener parks the whole process in accept() the moment it is idle. The tests happened
    // to reach accept() only when a connection was already queued, so they never saw it; a daemon
    // started with nothing connecting to it does nothing but hang.
    SocketStream listener(fd);
    listener.set_nonblocking();
    return listener;
}

}  // namespace rw
