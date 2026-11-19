#pragma once

#include "xnet/tcp_server.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace xnet_test {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Socket {
public:
    explicit Socket(int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) : fd_(fd) {
        require(fd_ >= 0, "could not open loopback client socket");
    }

    ~Socket() { ::close(fd_); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const noexcept { return fd_; }

private:
    int fd_;
};

inline std::uint16_t waitForPort(const xnet::TcpServer& server, std::future<int>& result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto port = server.port()) return port;
        if (result.wait_for(std::chrono::milliseconds(2)) == std::future_status::ready) {
            throw std::runtime_error("server exited without publishing a listening port");
        }
    }
    throw std::runtime_error("server did not publish a listening port");
}

inline void sendAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "failed to send loopback test bytes");
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

inline void connectClient(int fd, std::uint16_t port) {
    const timeval timeout{3, 0};
    require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "could not set client read timeout");
    require(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
            "could not set client write timeout");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "could not connect to xnet loopback listener");
}

inline void checkNextBytes(int fd, std::string_view expected) {
    char chunk[16 * 1024];
    while (!expected.empty()) {
        const auto count = ::recv(fd, chunk, std::min(expected.size(), sizeof(chunk)), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "expected response bytes were not delivered");
        const auto size = static_cast<std::size_t>(count);
        require(expected.substr(0, size) == std::string_view(chunk, size),
                "response contained unaccepted or changed bytes");
        expected.remove_prefix(size);
    }
}

inline void checkResponse(int fd, std::string_view expected) {
    checkNextBytes(fd, expected);
    char byte;
    ssize_t count;
    do {
        count = ::recv(fd, &byte, 1, 0);
    } while (count < 0 && errno == EINTR);
    require(count == 0, "connection did not close after the expected response");
}

template <typename Test>
void withServer(xnet::ServerOptions options, xnet::ServerCallbacks callbacks, Test&& test) {
    xnet::TcpServer server(std::move(options), std::move(callbacks));
    std::packaged_task<int()> run([&] { return server.run(); });
    auto result = run.get_future();
    std::thread server_thread(std::move(run));
    try {
        test(server, waitForPort(server, result));
    } catch (...) {
        server.stop();
        server_thread.join();
        throw;
    }
    server.stop();
    server_thread.join();
    require(result.get() == 0, "server run did not succeed");
}

} // namespace xnet_test
