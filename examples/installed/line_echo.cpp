#include "server_main.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main() {
    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [](const xnet::Connection::Ptr& connection) {
        // 每条连接保留自己的半行，直到收到换行符或连接关闭。
        connection->setReadCallback(
            [line = std::string{}](const xnet::Connection::Ptr& peer, std::string_view bytes) mutable {
                constexpr std::size_t max_line_length = 1024;
                for (char byte : bytes) {
                    if (byte == '\n') {
                        const auto reply = "line: " + line + '\n';
                        if (peer->send(std::vector<char>(reply.begin(), reply.end())) !=
                            xnet::SendResult::Queued) {
                            peer->close();
                            return;
                        }
                        line.clear();
                    } else if (line.size() == max_line_length) {
                        peer->close();
                        return;
                    } else {
                        line.push_back(byte);
                    }
                }
            });
    };
    return runExample(std::move(callbacks));
}
