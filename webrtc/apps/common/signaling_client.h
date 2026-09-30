// Newline-delimited JSON over TCP signaling client (protocol: apps/signaling/signaling_server.py).
// Plain POSIX sockets — no WebSocket dependency. Blocking receive loop on its own thread,
// thread-safe Send().
#ifndef P5G_APPS_COMMON_SIGNALING_CLIENT_H
#define P5G_APPS_COMMON_SIGNALING_CLIENT_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "json.hpp"

namespace p5g {

using json = nlohmann::json;

class SignalingClient {
 public:
  using Handler = std::function<void(const json&)>;

  // Bounded retry: the signaling server may come up a little later than the apps.
  bool Connect(const std::string& host, int port, int max_attempts = 60, int backoff_ms = 500) {
    for (int a = 1; a <= max_attempts; ++a) {
      if (TryConnectOnce(host, port)) return true;
      if (a < max_attempts) std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    }
    return false;
  }

  void Start(Handler on_message) {
    on_message_ = std::move(on_message);
    running_ = true;
    rx_thread_ = std::thread([this] { RecvLoop(); });
  }

  bool Send(const json& msg) {
    const std::string line = msg.dump() + "\n";
    std::lock_guard<std::mutex> lk(tx_mu_);
    if (fd_ < 0) return false;
    ssize_t off = 0;
    const ssize_t n = static_cast<ssize_t>(line.size());
    while (off < n) {
      const ssize_t w = ::send(fd_, line.data() + off, n - off, MSG_NOSIGNAL);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) return false;
      off += w;
    }
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    if (rx_thread_.joinable()) rx_thread_.join();
    std::lock_guard<std::mutex> lk(tx_mu_);
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  ~SignalingClient() { Stop(); }

 private:
  bool TryConnectOnce(const std::string& host, int port, int timeout_ms = 2000) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = false;
    if (fd >= 0) {
      const int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        const int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
        if (rc == 0) {
          ok = true;
        } else if (errno == EINPROGRESS) {
          fd_set wf;
          FD_ZERO(&wf);
          FD_SET(fd, &wf);
          timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
          if (::select(fd + 1, nullptr, &wf, nullptr, &tv) > 0) {
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            ok = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0;
          }
        }
        if (ok && ::fcntl(fd, F_SETFL, flags) < 0) ok = false;  // back to blocking for recv loop
      }
    }
    ::freeaddrinfo(res);
    if (!ok) {
      if (fd >= 0) ::close(fd);
      return false;
    }
    fd_ = fd;
    return true;
  }

  void RecvLoop() {
    std::string buf;
    char chunk[4096];
    while (running_) {
      const ssize_t r = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (r <= 0) break;
      buf.append(chunk, static_cast<size_t>(r));
      size_t pos;
      while ((pos = buf.find('\n')) != std::string::npos) {
        const std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        if (line.empty()) continue;
        try {
          on_message_(json::parse(line));
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[p5g][signaling] bad message ignored: %s\n", e.what());
        }
      }
    }
  }

  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread rx_thread_;
  std::mutex tx_mu_;
  Handler on_message_;
};

}  // namespace p5g

#endif  // P5G_APPS_COMMON_SIGNALING_CLIENT_H
