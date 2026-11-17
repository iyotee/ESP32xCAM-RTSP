#include "net/Socket.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <esp32-hal-log.h>

namespace net {
namespace {

bool setNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void setIntOption(int fd, int level, int name, int value) {
  if (setsockopt(fd, level, name, &value, sizeof(value)) != 0) {
    log_w("setsockopt(%d, %d) failed: errno %d", level, name, errno);
  }
}

inline bool isTransient(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == EINTR; }

Socket bindSocket(int type, uint16_t port) {
  Socket s(socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP));
  if (!s) {
    log_e("socket() failed: errno %d", errno);
    return Socket();
  }
  setIntOption(s.fd(), SOL_SOCKET, SO_REUSEADDR, 1);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s.fd(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || !setNonBlocking(s.fd())) {
    log_e("bind(%u) failed: errno %d", port, errno);
    return Socket();
  }
  return s;
}

}  // namespace

void Socket::reset(int fd) {
  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
  }
  fd_ = fd;
}

Socket listenTcp(uint16_t port, int backlog) {
  Socket s = bindSocket(SOCK_STREAM, port);
  if (s && listen(s.fd(), backlog) != 0) {
    log_e("listen(%u) failed: errno %d", port, errno);
    return Socket();
  }
  return s;
}

Socket bindUdp(uint16_t port) { return bindSocket(SOCK_DGRAM, port); }

Socket acceptClient(const Socket &listener, sockaddr_in *peer) {
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  Socket s(accept(listener.fd(), reinterpret_cast<sockaddr *>(&addr), &len));
  if (!s) {
    if (!isTransient(errno)) log_w("accept() failed: errno %d", errno);
    return Socket();
  }
  if (!setNonBlocking(s.fd())) {
    log_w("cannot make socket non-blocking: errno %d", errno);
    return Socket();
  }
  setIntOption(s.fd(), IPPROTO_TCP, TCP_NODELAY, 1);
  // lwIP's default ~5.7 KB send buffer caps throughput at snd_buf/RTT: on a
  // high-latency link that is a few KB/s. Give streaming sockets more room.
  int sndbuf = 65535;
  setsockopt(s.fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  setIntOption(s.fd(), SOL_SOCKET, SO_KEEPALIVE, 1);
  setIntOption(s.fd(), IPPROTO_TCP, TCP_KEEPIDLE, 10);
  setIntOption(s.fd(), IPPROTO_TCP, TCP_KEEPINTVL, 5);
  setIntOption(s.fd(), IPPROTO_TCP, TCP_KEEPCNT, 3);
  if (peer != nullptr) *peer = addr;
  return s;
}

IoResult sendSome(const Socket &s, const void *data, size_t len) {
  if (len == 0) return {IoStatus::kOk, 0};
  const ssize_t n = send(s.fd(), data, len, MSG_DONTWAIT);
  if (n > 0) return {IoStatus::kOk, size_t(n)};
  if (n < 0 && (isTransient(errno) || errno == ENOMEM)) return {IoStatus::kWouldBlock, 0};
  return {IoStatus::kError, 0};
}

IoResult recvSome(const Socket &s, void *buf, size_t len) {
  if (len == 0) return {IoStatus::kWouldBlock, 0};
  const ssize_t n = recv(s.fd(), buf, len, MSG_DONTWAIT);
  if (n > 0) return {IoStatus::kOk, size_t(n)};
  if (n == 0) return {IoStatus::kClosed, 0};
  if (isTransient(errno)) return {IoStatus::kWouldBlock, 0};
  return {IoStatus::kError, 0};
}

IoResult sendTo(const Socket &s, const void *data, size_t len, const sockaddr_in &to) {
  const ssize_t n = sendto(s.fd(), data, len, MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
  if (n == ssize_t(len)) return {IoStatus::kOk, len};
  if (n < 0 && (isTransient(errno) || errno == ENOMEM)) return {IoStatus::kWouldBlock, 0};
  return {IoStatus::kError, 0};
}

void formatIp(uint32_t addrNetworkOrder, char *out, size_t outSize) {
  const uint8_t *b = reinterpret_cast<const uint8_t *>(&addrNetworkOrder);
  snprintf(out, outSize, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

}  // namespace net
