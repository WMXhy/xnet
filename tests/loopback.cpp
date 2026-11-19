#include "xnet/connection.hpp"
#include "xnet/network_buffer.hpp"
#include "xnet/tcp_server.hpp"

#include "socket_test.hpp"

#include <iostream>
#include <memory>
#include <vector>

void checkClosedConnection(const xnet::Connection::Ptr& connection);
void checkNetworkBufferMoves();

namespace {

using namespace xnet_test;

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
        require(connection->send(std::vector<char>(bytes.begin(), bytes.end())) == xnet::SendResult::Queued,
                "echo bytes were not accepted");
    };
    callbacks.on_read_end = [&](const xnet::Connection::Ptr&) { ++read_ends; };
    callbacks.on_close = [&](const xnet::Connection::Ptr& connection) {
        ++closes;
        connection->close();
    };

    withServer({}, std::move(callbacks), [](xnet::TcpServer&, std::uint16_t port) {
        Socket client;
        const int fd = client.fd();
        connectClient(fd, port);

        const std::string payload{'x', '\0', static_cast<char>(0xff), 'n', 'e', 't', '\n'};
        const std::string_view bytes(payload);
        sendAll(fd, bytes.substr(0, 3));
        sendAll(fd, bytes.substr(3));
        require(::shutdown(fd, SHUT_WR) == 0, "could not half-close the client");
        checkResponse(fd, bytes);
    });

    checkClosedConnection(retained);
    require(opens == 1 && read_ends == 1 && closes == 1, "unexpected connection callback counts");
}

void testReadEndResponse() {
    xnet::ServerCallbacks callbacks;
    callbacks.on_read_end = [](const xnet::Connection::Ptr& connection) {
        require(connection->post([] {}), "read-end callback could not post to its owning loop");
        require(connection->send({'e', 'o', 'f'}) == xnet::SendResult::Queued,
                "inline work closed the connection before the read-end callback responded");
    };

    withServer({}, std::move(callbacks), [](xnet::TcpServer&, std::uint16_t port) {
        Socket client;
        connectClient(client.fd(), port);
        require(::shutdown(client.fd(), SHUT_WR) == 0, "could not half-close an idle connection");
        checkResponse(client.fd(), "eof");
    });
}

void testQuiesce() {
    std::promise<xnet::Connection::Ptr> pending_work;
    auto connection_ready = pending_work.get_future();
    std::promise<void> read_ended;
    auto read_end_ready = read_ended.get_future();

    xnet::ServerCallbacks callbacks;
    callbacks.on_bytes = [&](const xnet::Connection::Ptr& connection, std::string_view bytes) {
        require(bytes == "?", "unexpected request for quiesce test");
        connection->beginWork();
        pending_work.set_value(connection);
    };
    callbacks.on_read_end = [&](const xnet::Connection::Ptr&) { read_ended.set_value(); };

    withServer({}, std::move(callbacks), [&](xnet::TcpServer& server, std::uint16_t port) {
        Socket client;
        connectClient(client.fd(), port);
        sendAll(client.fd(), "?");
        require(::shutdown(client.fd(), SHUT_WR) == 0, "could not half-close the pending request");
        require(connection_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                "server did not start processing the request");
        const auto connection = connection_ready.get();
        require(read_end_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                "server did not see the client half-close before work finished");

        std::promise<void> quiesced;
        auto worker = std::async(std::launch::async, [connection, ready = quiesced.get_future()]() mutable {
            require(ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                    "worker never received the quiesce signal");
            ready.get();

            auto complete_work = std::make_shared<std::packaged_task<void()>>([connection] {
                require(connection->send({'o', 'k'}) == xnet::SendResult::Queued,
                        "quiesce rejected the worker response");
                connection->finishWork();
            });
            auto finished = complete_work->get_future();
            require(connection->post([complete_work] { (*complete_work)(); }),
                    "quiesce stopped the I/O loop before application work finished");
            require(finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                    "posted work did not run during quiesce");
            finished.get();
        });

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

            quiesced.set_value();
            worker.get();
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
        testReadEndResponse();
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
