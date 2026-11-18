# xnet

[详细使用指南](docs/usage.zh-CN.md) · [English](README.md)

xnet 是面向 Linux 和 WSL 2 的 C++17 IPv4 TCP 服务端库。它使用非阻塞套接字和 epoll，可在一个或多个 I/O 线程上处理连接。应用程序通过回调接收原始字节，并自行定义协议。

## 构建和测试

需要支持 C++17 的编译器和 CMake 3.16 或更新版本。库使用系统线程，无需第三方依赖。

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
```

测试覆盖二进制回环回显、TCP 半关闭及停机时发完待发送响应，并验证多个翻译单元同时包含公开头文件后仍能成功链接。仅构建库时可指定 `-DXNET_BUILD_TESTS=OFF`。

## 接入 CMake 项目

将 xnet 源码放到 `third_party/xnet`，然后在你的项目的 `CMakeLists.txt` 中加入以下内容。

```cmake
cmake_minimum_required(VERSION 3.16)
project(echo LANGUAGES CXX)

add_subdirectory(third_party/xnet)
add_executable(echo main.cpp)
target_link_libraries(echo PRIVATE xnet::xnet)
```

## 回显服务端

将示例保存为接入项目的 `main.cpp`。运行构建出的 `echo`，再从另一终端执行 `nc 127.0.0.1 9000` 连接服务端。

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
        connection->send(std::vector<char>(bytes.begin(), bytes.end()));
    };

    xnet::TcpServer server(options, std::move(callbacks));
    return server.run();
}
```

## 回调和停机

- `on_open`、`on_bytes`、`on_read_end` 和 `on_close` 在连接所属的 I/O 线程上运行。不同连接的回调可能并发执行。
- `on_bytes` 收到的字节视图只在当前回调内有效。如果需要在回调结束后访问这些字节，应先复制。解析 TCP 字节流时，需考虑任意分片边界。
- 在线程池等其他线程处理请求期间，可在 I/O 回调中调用 `beginWork()` 暂时保留连接，把响应交给 `send()` 后再调用 `finishWork()`。`on_read_end` 表示对端已结束发送。
- `run()` 会阻塞调用线程。需要有序停机时，从单独的管理线程调用 `stop()`，并在销毁服务端之前等待运行 `run()` 的线程退出。`stop()` 停止接受新连接和读取字节，在 I/O 线程仍运行时执行可选的停机回调，随后排空待发送数据。发送超时从停机回调返回后开始计时。请在该回调结束后再调用同一服务端的 run 或 stop 方法。
