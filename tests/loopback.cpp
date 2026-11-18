#include "xnet/connection.hpp"
#include "xnet/network_buffer.hpp"
#include "xnet/tcp_server.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

void checkClosedConnection(const xnet::Connection::Ptr& connection);
void checkNetworkBufferMoves();

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class ClientSocket {
public:
    ClientSocket() : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        require(fd_ >= 0, "could not open loopback client socket");
    }

    ~ClientSocket() { ::close(fd_); }

    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    int fd() const noexcept { return fd_; }

private:
    int fd_;
};

std::uint16_t waitForPort(const xnet::TcpServer& server, std::future<int>& result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto port = server.port()) return port;
        if (result.wait_for(std::chrono::milliseconds(2)) == std::future_status::ready) {
            throw std::runtime_error("server exited without publishing a listening port");
        }
    }
    throw std::runtime_error("server did not publish a listening port");
}

void sendAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "failed to send loopback test bytes");
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

void connectClient(int fd, std::uint16_t port) {
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

void checkResponse(int fd, std::string_view expected) {
    std::string response;
    char buffer[64];
    while (true) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "failed to read response before timeout");
        if (count == 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
        require(response.size() <= expected.size(), "received extra bytes in response");
    }
    require(response == expected, "response changed or lost bytes");
}

template <typename Test>
void withServer(xnet::ServerCallbacks callbacks, Test&& test) {
    xnet::TcpServer server(xnet::ServerOptions{}, std::move(callbacks));
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

void testLoopback() {
    xnet::Connection::Ptr retained;
    int opens = 0;
    int read_ends = 0;
    int closes = 0;

    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [&](const xnet::Connection::Ptr& connection) {
        retained = connection;
        ++opens;
    };
    callbacks.on_bytes = [](const xnet::Connection::Ptr& connection, std::string_view bytes) {
        connection->send(std::vector<char>(bytes.begin(), bytes.end()));
    };
    callbacks.on_read_end = [&](const xnet::Connection::Ptr&) { ++read_ends; };
    callbacks.on_close = [&](const xnet::Connection::Ptr&) { ++closes; };

    withServer(std::move(callbacks), [](xnet::TcpServer&, std::uint16_t port) {
        ClientSocket client;
        const int fd = client.fd();
        connectClient(fd, port);

        const std::string payload{'x', '\0', static_cast<char>(0xff), 'n', 'e', 't', '\n'};
        const std::string_view bytes(payload);
        sendAll(fd, bytes.substr(0, 3));
        sendAll(fd, bytes.substr(3));
        require(::shutdown(fd, SHUT_WR) == 0, "could not half-close the client");
        checkResponse(fd, bytes);
    });

    require(opens == 1 && read_ends == 1 && closes == 1, "unexpected connection callback counts");
    checkClosedConnection(retained);
}

void testQuiesce() {
    std::promise<xnet::Connection::Ptr> pending_work;
    auto connection_ready = pending_work.get_future();

    xnet::ServerCallbacks callbacks;
    callbacks.on_bytes = [&](const xnet::Connection::Ptr& connection, std::string_view bytes) {
        require(bytes == "?", "unexpected request for quiesce test");
        connection->beginWork();
        pending_work.set_value(connection);
    };

    withServer(std::move(callbacks), [&](xnet::TcpServer& server, std::uint16_t port) {
        ClientSocket client;
        connectClient(client.fd(), port);
        sendAll(client.fd(), "?");
        require(connection_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                "server did not start processing the request");
        const auto connection = connection_ready.get();

        bool hook_called = false;
        server.stop([&] {
            hook_called = true;
            require(server.port() == 0, "listener still accepting after quiesce");
            require(server.run() == 1, "run was reentered through the shutdown hook");

            bool recursive_stop_rejected = false;
            try {
                server.stop();
            } catch (const std::logic_error&) {
                recursive_stop_rejected = true;
            }
            require(recursive_stop_rejected, "stop was reentered through the shutdown hook");

            // Simulate work completing on the owning loop while the application is draining.
            auto complete_work = std::make_shared<std::packaged_task<void()>>([connection] {
                connection->send(std::vector<char>{'o', 'k'});
                connection->finishWork();
            });
            auto finished = complete_work->get_future();
            require(connection->post([complete_work] { (*complete_work)(); }),
                    "quiesce stopped the I/O loop before application work finished");
            require(finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                    "posted work did not run during quiesce");
            finished.get();
        });

        require(hook_called, "stop did not call the quiesce hook");
        checkResponse(client.fd(), "ok");
    });
}

} // namespace

int main(int argc, char* argv[]) try {
    if (argc == 2 && std::string_view(argv[1]) == "quiesce") {
        testQuiesce();
    } else {
        checkNetworkBufferMoves();
        testLoopback();
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
