# xnet

[Detailed usage guide](docs/usage.md) · [简体中文](README.zh-CN.md)

xnet is a C++17 library for IPv4 TCP servers on Linux and WSL 2. It uses nonblocking sockets and epoll to handle connections across one or more I/O threads. Applications receive raw bytes through callbacks and define their own protocol.

## Build and test

You need a C++17 compiler and CMake 3.16 or newer. The library uses system threads and has no third-party dependencies.

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
```

The tests cover binary loopback, TCP half-close, and the delivery of pending replies during shutdown. The test executable also verifies that the public headers can be included and linked from multiple translation units. Use `-DXNET_BUILD_TESTS=OFF` to build just the library.

## Use xnet in a CMake project

Place the xnet source in `third_party/xnet` and add the following to your project's `CMakeLists.txt`.

```cmake
cmake_minimum_required(VERSION 3.16)
project(echo LANGUAGES CXX)

add_subdirectory(third_party/xnet)
add_executable(echo main.cpp)
target_link_libraries(echo PRIVATE xnet::xnet)
```

## Echo server

Save this example as `main.cpp` in the consuming project. Run the built `echo` executable, then connect to it with `nc 127.0.0.1 9000` from another terminal.

```cpp
#include <xnet/tcp_server.hpp>

#include <string_view>
#include <utility>
#include <vector>

int main() {
    xnet::ServerOptions options;
    options.address = "127.0.0.1";
    options.port = 9000;

    xnet::ServerCallbacks callbacks;
    callbacks.on_bytes = [](const xnet::Connection::Ptr& connection,
                            std::string_view bytes) {
        if (connection->send(std::vector<char>(bytes.begin(), bytes.end())) ==
            xnet::SendResult::QueueFull) {
            connection->close();
        }
    };

    xnet::TcpServer server(options, std::move(callbacks));
    return server.run();
}
```

## Callbacks and shutdown

- `on_open`, `on_bytes`, `on_read_end`, and `on_close` run on the connection's I/O thread. Callbacks for different connections can run concurrently.
- The `on_bytes` view is valid only during the callback. Copy the bytes if they need to outlive it, and parse the stream with arbitrary chunk boundaries in mind. Handle `SendResult::QueueFull` when the connection reaches its pending-byte limit.
- To keep a connection open while processing a request off-thread, call `beginWork()` in the I/O callback, queue any reply with `send()`, and then call `finishWork()`. The `on_read_end` callback signals that the peer has finished sending.
- `run()` blocks its caller. For an orderly shutdown, call `stop()` from a separate management thread and join the thread running `run()` before destroying the server. `stop()` stops accepting connections and reading bytes, runs the optional hook while the I/O threads are still active, and then drains pending writes. The flush timeout starts when the hook returns. Keep calls to that server's run and stop methods outside the hook.
