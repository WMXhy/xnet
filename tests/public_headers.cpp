#include "xnet/connection.hpp"
#include "xnet/network_buffer.hpp"
#include "xnet/tcp_server.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

void checkClosedConnection(const xnet::Connection::Ptr& connection) {
    if (!connection || !connection->isClosed()) {
        throw std::runtime_error("retained connection remains open after server teardown");
    }

    const auto id = connection->id();
    connection->send(std::vector<char>{'x'});
    connection->close();
    if (!connection->isClosed() || connection->id() != id) {
        throw std::runtime_error("closed connection changed after late send or close");
    }
}

void checkNetworkBufferMoves() {
    xnet::NetworkBuffer source;
    source.append("abc", 3);
    source.retrieve(1);

    xnet::NetworkBuffer moved(std::move(source));
    if (source.readable() != 0 || std::string_view(moved.peek(), moved.readable()) != "bc") {
        throw std::runtime_error("buffer move construction lost data or left stale indices");
    }

    source.append("z", 1);
    source = std::move(moved);
    if (moved.readable() != 0 || std::string_view(source.peek(), source.readable()) != "bc") {
        throw std::runtime_error("buffer move assignment lost data or left stale indices");
    }

    moved.append("q", 1);
    if (std::string_view(moved.peek(), moved.readable()) != "q") {
        throw std::runtime_error("moved-from buffer could not be reused");
    }
}
