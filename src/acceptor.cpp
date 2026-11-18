#include "acceptor.hpp"
#include "event_loop.hpp"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace xnet::detail {
namespace {

[[noreturn]] void throwSocketError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

SocketFd listenSocket(const std::string& address, std::uint16_t port, int backlog) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        throw std::invalid_argument("Invalid IPv4 listen address: " + address);
    }

    SocketFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (socket.get() < 0) throwSocketError("socket");

    const int reuse = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throwSocketError("setsockopt(SO_REUSEADDR)");
    }
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        throwSocketError("bind");
    }
    if (::listen(socket.get(), backlog) < 0) throwSocketError("listen");
    return socket;
}

std::uint16_t boundPort(int fd) {
    sockaddr_in endpoint{};
    socklen_t size = sizeof(endpoint);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&endpoint), &size) < 0) {
        throwSocketError("getsockname");
    }
    return ntohs(endpoint.sin_port);
}

} // namespace

Acceptor::Acceptor(EventLoop& loop, const std::string& address, std::uint16_t port,
                   int backlog, AcceptCallback on_accept)
    : loop_(loop),
      socket_(listenSocket(address, port, backlog)),
      on_accept_(std::move(on_accept)),
      port_(boundPort(socket_.get())) {
    loop_.addEvent(socket_.get(), EPOLLIN, [this](std::uint32_t events) {
        if (events & EPOLLIN) acceptOne();
    });
}

Acceptor::~Acceptor() {
    // Normal shutdown removes the registration on the listener thread.
    // If that loop has already failed, closing the socket removes it from epoll.
    if (loop_.isInLoopThread()) loop_.delEvent(socket_.get());
}

void Acceptor::acceptOne() {
    int fd;
    do {
        fd = ::accept4(socket_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) on_accept_(SocketFd(fd));
}

} // namespace xnet::detail
