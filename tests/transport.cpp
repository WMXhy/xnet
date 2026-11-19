#include "xnet/connection.hpp"
#include "xnet/network_buffer.hpp"
#include "socket_test.hpp"

#include <poll.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace xnet_test;
using xnet::Connection;
using xnet::SendResult;

std::string makePayload(std::size_t size) {
    std::string payload(size, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 31 + i / 257) % 256);
    }
    return payload;
}

void testShortWrite() {
    int pair[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0,
            "could not create the short-write socket pair");
    Socket writer(pair[0]);
    Socket reader(pair[1]);
    const int buffer_size = 4096;
    require(::setsockopt(writer.fd(), SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0,
            "could not limit the socket send buffer");

    const auto payload = makePayload(256 * 1024);
    xnet::NetworkBuffer output;
    output.append(payload.data(), payload.size());

    const auto first_write = output.write_to_fd(writer.fd());
    require(first_write > 0 && static_cast<std::size_t>(first_write) < payload.size(),
            "socket did not force a short write");
    require(output.readable() == payload.size() - static_cast<std::size_t>(first_write),
            "short write consumed the wrong number of bytes");
    require(output.write_to_fd(writer.fd()) == 0, "full socket did not report zero progress");

    std::string received;
    received.reserve(payload.size());
    char chunk[1536];
    while (received.size() < payload.size()) {
        const auto count = ::recv(reader.fd(), chunk, sizeof(chunk), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "short-write retry stopped making progress");
        received.append(chunk, static_cast<std::size_t>(count));
        require(output.write_to_fd(writer.fd()) >= 0, "short-write retry failed");
    }
    require(output.readable() == 0 && received == payload,
            "short-write retries lost, duplicated, or changed bytes");
}

void testSlowReader() {
    constexpr std::size_t payload_size = 8 * 1024 * 1024;
    const auto payload = makePayload(payload_size);
    std::promise<Connection::Ptr> queued;
    auto response_queued = queued.get_future();

    xnet::ServerOptions options;
    options.max_pending_bytes = payload.size();
    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [&](const Connection::Ptr& connection) {
        require(connection->send(std::vector<char>(payload.begin(), payload.end())) == SendResult::Queued,
                "slow-reader output was not queued");
        queued.set_value(connection);
    };

    withServer(options, std::move(callbacks), [&](xnet::TcpServer&, std::uint16_t port) {
        Socket client;
        const int fd = client.fd();
        const int receive_buffer = 64 * 1024;
        require(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0,
                "could not limit the slow reader's receive buffer");
        connectClient(fd, port);
        require(response_queued.wait_for(3s) == std::future_status::ready,
                "large response was not accepted");
        const auto connection = response_queued.get();

        // 等到套接字实际收到数据，暂不读取以保持接收窗口占用。
        pollfd readable{fd, POLLIN, 0};
        int result;
        do {
            result = ::poll(&readable, 1, 3000);
        } while (result < 0 && errno == EINTR);
        require(result == 1 && (readable.revents & POLLIN), "large response never began writing");
        require(connection->send(std::vector<char>(payload_size, '!')) == SendResult::QueueFull,
                "client did not force a short write with bytes still queued");

        require(::shutdown(fd, SHUT_WR) == 0, "could not half-close slow reader");
        checkResponse(fd, payload);
    });
}

void testQueueFull() {
    xnet::ServerOptions options;
    options.max_pending_bytes = 4;
    std::promise<Connection::Ptr> opened;
    auto connection_ready = opened.get_future();
    std::promise<void> resume_io;
    auto io_released = resume_io.get_future();

    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [&](const Connection::Ptr& connection) {
        opened.set_value(connection);
        require(io_released.wait_for(3s) == std::future_status::ready,
                "socket I/O was never released");
    };
    callbacks.on_bytes = [](const Connection::Ptr& connection, std::string_view bytes) {
        require(bytes == "?", "unexpected request for queue budget test");
        require(connection->send({'e', 'c', 'h', 'o'}) == SendResult::Queued,
                "written bytes did not release send queue budget");
        require(connection->send({'!'}) == SendResult::QueueFull,
                "owner-thread send exceeded queue budget");
    };

    withServer(options, std::move(callbacks), [&](xnet::TcpServer&, std::uint16_t port) {
        Socket client;
        connectClient(client.fd(), port);
        require(connection_ready.wait_for(3s) == std::future_status::ready,
                "queue test connection did not open");
        const auto connection = connection_ready.get();

        const auto too_large = connection->send({'1', '2', '3', '4', '5'});
        const auto first = connection->send({'e', 'c', 'h', 'o'});
        const auto second = connection->send({'!'});
        resume_io.set_value();
        require(too_large == SendResult::QueueFull, "oversized send exceeded queue budget");
        require(first == SendResult::Queued, "send at queue limit was rejected");
        require(second == SendResult::QueueFull,
                "bytes waiting in the cross-thread queue were not counted");
        checkNextBytes(client.fd(), "echo");

        for (int i = 0; i < 2; ++i) {
            sendAll(client.fd(), "?");
            checkNextBytes(client.fd(), "echo");
        }
        require(::shutdown(client.fd(), SHUT_WR) == 0, "could not half-close queue test socket");
        checkResponse(client.fd(), "");
    });
}

std::size_t descriptorCount() {
    using Iterator = std::filesystem::directory_iterator;
    return static_cast<std::size_t>(std::distance(Iterator("/proc/self/fd"), Iterator{}));
}

std::uint16_t reservePort(int fd) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "could not bind a port for the startup conflict test");
    require(::listen(fd, 1) == 0, "could not reserve the port");
    socklen_t size = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
            "could not get the reserved port");
    return ntohs(address.sin_port);
}

void testStartupFailure() {
    const auto initial_fds = descriptorCount();
    xnet::ServerOptions options;
    options.io_threads = 2;
    {
        Socket occupied;
        options.port = reservePort(occupied.fd());
        const auto occupied_fds = descriptorCount();

        for (int i = 0; i < 3; ++i) {
            xnet::TcpServer server(options);
            require(server.run() == 1, "startup with an occupied port did not fail");
            require(server.port() == 0, "failed startup published a listening port");
            require(descriptorCount() == occupied_fds, "failed startup leaked descriptors");
        }
    }

    withServer(options, {}, [&](xnet::TcpServer&, std::uint16_t port) {
        require(port == options.port, "server could not rebind the released port");
        Socket client;
        connectClient(client.fd(), port);
        require(::shutdown(client.fd(), SHUT_WR) == 0, "could not half-close rebound connection");
        checkResponse(client.fd(), "");
    });
    require(descriptorCount() == initial_fds, "start and stop leaked descriptors");
}

void testConcurrentShutdown() {
    std::promise<Connection::Ptr> opened;
    auto connection_ready = opened.get_future();
    std::atomic<std::size_t> executed{0};
    int closes = 0;
    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [&](const Connection::Ptr& connection) {
        connection->beginWork();
        opened.set_value(connection);
    };
    callbacks.on_close = [&](const Connection::Ptr&) { ++closes; };

    withServer({}, std::move(callbacks), [&](xnet::TcpServer& server, std::uint16_t port) {
        Socket client;
        connectClient(client.fd(), port);
        require(connection_ready.wait_for(3s) == std::future_status::ready,
                "shutdown test connection did not open");
        const auto connection = connection_ready.get();

        std::promise<void> quiesced;
        std::promise<bool> first_post;
        auto first_posted = first_post.get_future();
        auto submitter = std::async(std::launch::async,
                                    [connection, &executed, &first_post, ready = quiesced.get_future()] {
            require(ready.wait_for(3s) == std::future_status::ready,
                    "submission worker never received the quiesce signal");
            const auto submit = [&] { return connection->post([&executed] { ++executed; }); };
            const bool first = submit();
            const auto first_send = connection->send({'x'});
            first_post.set_value(first && first_send == SendResult::Queued);
            require(first && first_send == SendResult::Queued,
                    "work submitted from the worker during quiesce was rejected");

            std::size_t accepted_posts = 1;
            std::size_t accepted_sends = 1;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < deadline) {
                const bool posted = submit();
                const auto send = connection->send({'x'});
                require(send != SendResult::QueueFull, "one-byte send unexpectedly filled the queue");
                if (posted) ++accepted_posts;
                if (send == SendResult::Queued) ++accepted_sends;
                if (!posted) {
                    require(send == SendResult::Closed, "send admission reopened after post refusal");
                    return std::make_pair(accepted_posts, accepted_sends);
                }
                std::this_thread::sleep_for(1ms);
            }
            throw std::runtime_error("shutdown did not reject subsequent submissions");
        });

        server.stop([&] {
            quiesced.set_value();
            require(first_posted.wait_for(3s) == std::future_status::ready && first_posted.get(),
                    "quiesce did not admit the initial worker task");
        }, 200ms);

        const auto [accepted_posts, accepted_sends] = submitter.get();
        require(executed.load() == accepted_posts, "shutdown lost or repeated an accepted task");
        require(connection->isClosed(), "shutdown left the connection open");
        require(!connection->post([] {}), "closed connection accepted a task");
        require(connection->send({'!'}) == SendResult::Closed, "closed connection accepted bytes");
        connection->close();
        checkResponse(client.fd(), std::string(accepted_sends, 'x'));
    });
    require(closes == 1, "concurrent shutdown did not notify closure exactly once");
}

void testDrainTimeout() {
    constexpr std::size_t payload_size = 16 * 1024 * 1024;
    constexpr auto timeout = 125ms;
    std::promise<Connection::Ptr> queued;
    auto response_queued = queued.get_future();
    int closes = 0;

    xnet::ServerOptions options;
    options.max_pending_bytes = payload_size;
    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [&](const Connection::Ptr& connection) {
        require(connection->send(std::vector<char>(payload_size, 'x')) == SendResult::Queued,
                "drain-test output was not queued");
        queued.set_value(connection);
    };
    callbacks.on_close = [&](const Connection::Ptr&) { ++closes; };

    withServer(options, std::move(callbacks), [&](xnet::TcpServer& server, std::uint16_t port) {
        Socket client;
        const int receive_buffer = 4096;
        require(::setsockopt(client.fd(), SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0,
                "could not limit the drain-test receive buffer");
        connectClient(client.fd(), port);
        require(response_queued.wait_for(3s) == std::future_status::ready,
                "large response was not queued for the slow reader");
        const auto connection = response_queued.get();

        // 停机期间保持接收窗口占满，验证排空超时。
        const auto start = std::chrono::steady_clock::now();
        server.stop({}, timeout);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        require(elapsed >= timeout, "stop discarded pending socket output before the deadline");
        require(elapsed < 3s, "stop ignored the drain deadline with a slow reader");
        require(connection->isClosed(), "drain deadline left the connection open");
    });
    require(closes == 1, "forced close did not notify exactly once");
}

} // namespace

int main(int argc, char* argv[]) try {
    require(argc == 2, "expected one transport test name");
    const std::string_view test = argv[1];
    if (test == "short_write") {
        testShortWrite();
    } else if (test == "slow_reader") {
        testSlowReader();
    } else if (test == "queue_full") {
        testQueueFull();
    } else if (test == "startup_failure") {
        testStartupFailure();
    } else if (test == "concurrent_shutdown") {
        testConcurrentShutdown();
    } else if (test == "drain_timeout") {
        testDrainTimeout();
    } else {
        throw std::runtime_error("unknown transport test");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
