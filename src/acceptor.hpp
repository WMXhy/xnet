#pragma once

#include <unistd.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace xnet::detail {

class EventLoop;

class SocketFd {
public:
    explicit SocketFd(int fd) noexcept : fd_(fd) {}
    ~SocketFd() { if (fd_ >= 0) ::close(fd_); }

    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(const SocketFd&) = delete;
    SocketFd(SocketFd&& other) noexcept : fd_(other.release()) {}

    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_;
};

class Acceptor {
public:
    using AcceptCallback = std::function<void(SocketFd)>;

    Acceptor(EventLoop& loop, const std::string& address, std::uint16_t port,
             int backlog, AcceptCallback on_accept);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    std::uint16_t port() const noexcept { return port_; }

private:
    void acceptOne();

    EventLoop& loop_;
    SocketFd socket_;
    AcceptCallback on_accept_;
    std::uint16_t port_;
};

} // namespace xnet::detail
