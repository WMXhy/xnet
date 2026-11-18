# xnet usage guide

[Project README](../README.md) · [简体中文](usage.zh-CN.md)

## Build and test xnet

Run the commands in a Linux or WSL 2 shell with Git, a C++17 compiler, and CMake 3.16 or newer. xnet provides a static library for IPv4 TCP servers and uses system threads.

```sh
git clone https://github.com/WMXhy/xnet.git
cd xnet
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
```

A standalone build enables tests by default. Set `-DXNET_BUILD_TESTS=OFF` when you only need the library.

## Add xnet to an application

From the root of your application, put a copy of xnet in `third_party/xnet`. If you keep the source elsewhere, pass its path to `add_subdirectory` and specify a separate binary directory as the second argument.

```sh
mkdir -p third_party
git clone https://github.com/WMXhy/xnet.git third_party/xnet
```

Create this `CMakeLists.txt` and save the example below as `main.cpp`. The `xnet::xnet` target provides the include directory, C++17 requirement, and thread dependency. xnet's own tests are off by default when it is a subproject.

```cmake
cmake_minimum_required(VERSION 3.16)
project(line_echo LANGUAGES CXX)

add_subdirectory(third_party/xnet)
add_executable(line_echo main.cpp)
target_link_libraries(line_echo PRIVATE xnet::xnet)
```

## Run a line echo server

A TCP read can deliver part of a line or several lines together. The example gives each connection its own buffer, echoes complete newline-terminated lines, and closes a connection if a line exceeds 1,024 bytes before its newline. A partial line at EOF is discarded when the connection closes.

```cpp
#include <xnet/tcp_server.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

int main() {
    xnet::ServerOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 2;
    options.backlog = 64;

    xnet::ServerCallbacks callbacks;
    callbacks.on_open = [](const xnet::Connection::Ptr& connection) {
        connection->setReadCallback(
            [line = std::string{}](const xnet::Connection::Ptr& peer,
                                   std::string_view bytes) mutable {
                constexpr std::size_t max_line_length = 1024;
                for (char byte : bytes) {
                    if (byte == '\n') {
                        line.push_back('\n');
                        peer->send(std::vector<char>(line.begin(), line.end()));
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

    xnet::TcpServer server(options, std::move(callbacks));
    std::packaged_task<int()> run([&] { return server.run(); });
    auto result = run.get_future();
    std::thread listener(std::move(run));

    std::uint16_t port = 0;
    while ((port = server.port()) == 0) {
        if (result.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
            listener.join();
            return result.get();
        }
    }

    std::cout << "Listening on 127.0.0.1:" << port << std::endl;
    std::string input;
    std::getline(std::cin, input);
    server.stop();
    listener.join();
    return result.get();
}
```

```sh
cmake -S . -B build
cmake --build build
./build/line_echo
```

In another terminal, run `nc 127.0.0.1 PORT` with `PORT` replaced by the printed port. Type a line and press Enter to see it echoed. Press Enter in the **server** terminal to stop it. To use a fixed port, set `options.port` before constructing the server.

## Configure the listener and callbacks

[`ServerOptions` and `ServerCallbacks`](../include/xnet/tcp_server.hpp) configure the listener.

| Option | Default | Use |
| --- | --- | --- |
| `address` | `127.0.0.1` | A numeric IPv4 address. Use `0.0.0.0` to bind all IPv4 interfaces. |
| `port` | `0` | A port from 0 to 65535. Use `0` for an OS-assigned port and read it with `server.port()` once the listener is ready. |
| `io_threads` | `1` | A positive number of I/O threads. Callbacks for connections on different threads may execute concurrently. |
| `backlog` | `5` | A positive listener backlog for pending connections. |

| Callback | When to use it |
| --- | --- |
| `on_open` | Initialize state for an accepted connection. Use `setReadCallback`, `setReadEndCallback`, or `setCloseCallback` on that I/O thread to replace the corresponding default for that connection. |
| `on_bytes` | Feed received bytes into an incremental parser. Its `std::string_view` lasts only for the callback. Copy any bytes you keep or pass to a worker. |
| `on_read_end` | Handle EOF after the peer closes its sending side. Decide what to do with any incomplete application message. The connection can still send pending replies. |
| `on_close` | Clean up application state associated with this connection. |

Callbacks for one connection run on its owning I/O thread. Synchronize access to state shared across connections or worker threads.

## Send replies and use worker threads

[`Connection::Ptr`](../include/xnet/connection.hpp) is a shared handle. `id()` identifies the connection within its server. `isClosed()` reports its current state. A retained handle can outlive the network connection.

- `send(std::vector<char>)` takes ownership of the bytes and queues them for transmission. Use an application-level acknowledgment if you need confirmation that the peer processed a reply.
- `close()` closes immediately on the owning I/O thread and requests a close when called elsewhere. Closing explicitly can discard pending output. Use `armIdle(timeout)` to start or reset an idle timer. The timer is opt-in and closes the connection immediately on expiry.
- `post(task)` schedules state updates on the owning I/O thread and reports whether the task was accepted. It can execute inline when called on that thread. An accepted task can run after the connection closes. Check `isClosed()` inside the task if it needs an open connection.

Keep I/O callbacks short. For slow work, parse a complete request, create owned data, and use your application's worker queue.

1. Call `beginWork()` on the connection's I/O thread before submitting the job.
2. Give the worker its own request data and a copy of the connection handle.
3. Queue any reply with `send()` before calling `finishWork()`. Pair every `beginWork()` with one `finishWork()`, including enqueue failure, cancellation, and error paths.

On a normal peer half-close, xnet keeps the connection open while tracked work or queued output remains. Your protocol can set its own frame-size and worker-queue limits, as the example does for line length.

## Stop the server cleanly

`run()` blocks and can be called once per server instance. It returns `0` after normal shutdown or a nonzero status on failure. Wait for `port()` to become nonzero before treating the listener as ready. If the address or port is unavailable, check the startup error and the return status.

Call `stop()` from a management thread outside the server's callbacks. For an application with a worker pool, use `server.stop(drain_workers, std::chrono::seconds(5))`, where `drain_workers` waits for work already submitted by the I/O callbacks.

1. xnet stops accepting connections and dispatching read events.
2. xnet runs the optional hook on the calling thread while the I/O loops can still process replies and posted tasks.
3. After the hook returns, xnet drains connections. The flush timeout starts at this step and bounds the socket-drain phase. The default is five seconds.
4. Once `stop()` returns, join the thread that called `run()` before destroying the server. Keep lifecycle calls for the same server outside the shutdown hook. If the hook throws, `stop()` cleans up first and then propagates the exception.

## Use the low-level buffer

Applications that manage their own sockets can include [`<xnet/network_buffer.hpp>`](../include/xnet/network_buffer.hpp). Use `append()` for bytes in a separate source range, inspect the readable prefix with `peek()` and `readable()`, and consume it with `retrieve(n)` where `n <= readable()`. A mutation can invalidate the pointer returned by `peek()`. `read_from_fd()` appends data from a socket and returns the byte count, `0` at EOF, or `-1` with `errno` set. On a nonblocking socket, `write_to_fd()` consumes bytes as the socket accepts them. Retry any pending bytes when the socket is writable again. xnet manages the transport buffers internally when you use `TcpServer` and `Connection`.
