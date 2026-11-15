// Thin RAII layer over lwIP BSD sockets. Everything is non-blocking: the whole
// firmware runs in one cooperative loop and must never wait on a peer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <lwip/sockets.h>

namespace net {

// Owns a socket descriptor; move-only, closes on destruction.
class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket() { reset(); }
  Socket(Socket &&other) noexcept : fd_(other.release()) {}
  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  explicit operator bool() const { return valid(); }

  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset(int fd = -1);

 private:
  int fd_ = -1;
};

enum class IoStatus : uint8_t { kOk, kWouldBlock, kClosed, kError };

struct IoResult {
  IoStatus status;
  size_t bytes;  // transferred bytes (kOk only)
};

// Listening TCP socket on all interfaces, non-blocking, SO_REUSEADDR.
Socket listenTcp(uint16_t port, int backlog);
// Bound UDP socket on all interfaces, non-blocking.
Socket bindUdp(uint16_t port);
// Accepts one pending connection (invalid Socket if none). The accepted socket
// is non-blocking, has TCP_NODELAY and keep-alive enabled.
Socket acceptClient(const Socket &listener, sockaddr_in *peer);

IoResult sendSome(const Socket &s, const void *data, size_t len);
IoResult recvSome(const Socket &s, void *buf, size_t len);
// One datagram; kWouldBlock also covers transient lwIP buffer exhaustion.
IoResult sendTo(const Socket &s, const void *data, size_t len, const sockaddr_in &to);

// Dotted-quad text form, `out` must hold 16 bytes.
void formatIp(uint32_t addrNetworkOrder, char *out, size_t outSize);

}  // namespace net
