#include "server_main.hpp"

#include <string_view>
#include <utility>
#include <vector>

int main() {
    xnet::ServerCallbacks callbacks;
    callbacks.on_bytes = [](const xnet::Connection::Ptr& connection, std::string_view bytes) {
        if (connection->send(std::vector<char>(bytes.begin(), bytes.end())) ==
            xnet::SendResult::QueueFull) {
            connection->close();
        }
    };
    return runExample(std::move(callbacks));
}
