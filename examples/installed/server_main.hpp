#pragma once

#include <xnet/tcp_server.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <thread>
#include <utility>

inline int runExample(xnet::ServerCallbacks callbacks) {
    xnet::TcpServer server(xnet::ServerOptions{}, std::move(callbacks));
    std::packaged_task<int()> run([&] { return server.run(); });
    auto result = run.get_future();
    std::thread listener(std::move(run));

    std::uint16_t port;
    while ((port = server.port()) == 0) {
        if (result.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
            listener.join();
            return result.get();
        }
    }

    std::cout << "Listening on 127.0.0.1:" << port << std::endl;
    std::cin.get();
    server.stop();
    listener.join();
    return result.get();
}
