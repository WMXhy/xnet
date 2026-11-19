# xnet 详细使用指南

[项目介绍](../README.zh-CN.md) · [English](usage.md)

## 构建和测试 xnet

请在 Linux 或 WSL 2 的终端中运行命令。环境需要 Git、支持 C++17 的编译器，以及 CMake 3.16 或更新版本。xnet 提供 IPv4 TCP 服务端静态库，使用系统线程。

```sh
git clone https://github.com/WMXhy/xnet.git
cd xnet
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
```

独立构建默认启用测试。仅构建库时，可以在配置阶段指定 `-DXNET_BUILD_TESTS=OFF`。

## 使用已安装的 CMake 包

先按上一节独立构建 xnet，再从 xnet 源码目录执行以下命令。命令会安装库、移动整个安装前缀，然后在独立工程中用搬迁后的包构建[两个示例](../examples/installed)。

```sh
demo_root=$(mktemp -d)
cmake --install build --prefix "$demo_root/xnet"
mv "$demo_root/xnet" "$demo_root/relocated-xnet"
cp -R examples/installed "$demo_root/consumer"
cmake -S "$demo_root/consumer" -B "$demo_root/consumer/build" \
    -DCMAKE_PREFIX_PATH="$demo_root/relocated-xnet"
cmake --build "$demo_root/consumer/build"
```

运行 `"$demo_root/consumer/build/xnet_echo"` 或 `"$demo_root/consumer/build/xnet_line_echo"`。程序会打印回环端口，在另一终端用 `nc 127.0.0.1 PORT` 连接，替换其中的端口号。第一个程序原样回显字节。第二个等到换行后返回完整行，并添加 `line: ` 前缀。在服务端终端按回车即可停机。

自己的工程同样使用 `find_package(xnet 0.1 CONFIG REQUIRED)`，并将目标链接到 `xnet::xnet`。安装包包含公开头文件、静态库、CMake 配置和版本文件。`CMAKE_INSTALL_LIBDIR` 和 `CMAKE_INSTALL_INCLUDEDIR` 应为相对于安装前缀的目录。独立构建默认启用安装，作为子项目时可通过 `-DXNET_INSTALL=ON` 显式启用。安装 Python 3.8 或更新版本后，CTest 会纳入安装包搬迁回归。

## 接入自己的 CMake 项目

在应用根目录下，将 xnet 源码放到 `third_party/xnet`。如果源码位于其他目录，可以给 `add_subdirectory` 传入源码路径，并用第二个参数指定独立的构建目录。

```sh
mkdir -p third_party
git clone https://github.com/WMXhy/xnet.git third_party/xnet
```

创建以下 `CMakeLists.txt`，再将下一节的示例保存为 `main.cpp`。链接 `xnet::xnet` 即可获得头文件路径、C++17 编译要求和线程依赖。作为子项目时，xnet 默认关闭自身的测试。

```cmake
cmake_minimum_required(VERSION 3.16)
project(line_echo LANGUAGES CXX)

add_subdirectory(third_party/xnet)
add_executable(line_echo main.cpp)
target_link_libraries(line_echo PRIVATE xnet::xnet)
```

## 运行按行回显服务端

一次 TCP 读取可能只收到半行，也可能同时收到多行。示例为每个连接维护独立的缓冲区，以换行符为边界回显完整行。换行前超过 1,024 字节时关闭连接。EOF 时剩余的半行随连接关闭而丢弃。

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
                        if (peer->send(std::vector<char>(line.begin(), line.end())) !=
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

在另一个终端执行 `nc 127.0.0.1 PORT`，把 `PORT` 替换成服务端打印的端口。输入一行并按回车即可看到回显。在**服务端**终端按回车可以停机。需要固定端口时，在构造服务端前设置 `options.port`。

## 配置监听与回调

使用 [`ServerOptions` 和 `ServerCallbacks`](../include/xnet/tcp_server.hpp) 配置服务端。

| 配置项 | 默认值 | 用法 |
| --- | --- | --- |
| `address` | `127.0.0.1` | 数字形式的 IPv4 监听地址。用 `0.0.0.0` 监听全部 IPv4 接口。 |
| `port` | `0` | 取值范围为 0 到 65535。设为 `0` 时由系统分配端口，监听就绪后通过 `server.port()` 读取。 |
| `io_threads` | `1` | I/O 线程数，须为正数。分配到不同线程的连接可以并行执行回调。 |
| `backlog` | `5` | 等待服务端接入的连接队列参数，须为正数。 |
| `max_pending_bytes` | 4 MiB | 单连接已接收但仍由 xnet 持有的待发送字节上限。设为 `0` 时只接受空发送。 |

| 回调 | 用法 |
| --- | --- |
| `on_open` | 初始化新连接的状态。在所属 I/O 线程调用 `setReadCallback`、`setReadEndCallback` 或 `setCloseCallback`，可为此连接替换相应的默认回调。 |
| `on_bytes` | 将收到的字节交给增量解析器。`std::string_view` 只在本次回调内有效，需要保留或交给其他线程时先复制。 |
| `on_read_end` | 处理对端结束发送产生的 EOF，并决定如何处置不完整的应用消息。此时仍可发完待处理的响应。 |
| `on_close` | 清理与该连接关联的应用状态。 |

同一连接的回调在其所属的 I/O 线程执行。连接之间或 I/O 线程与业务线程共用的数据需要同步访问。

## 发送响应与分配业务任务

[`Connection::Ptr`](../include/xnet/connection.hpp) 是共享连接句柄。`id()` 可在服务端实例内标识连接，`isClosed()` 用于查询当前关闭状态。持有的句柄可以比网络连接存活更久。

- `send(std::vector<char>)` 接管待发送数据。接收成功时返回 `Queued`，超过上限时返回 `QueueFull`，连接关闭或停止接收发送请求时返回 `Closed`。写入套接字的字节会释放发送额度。如果业务要确认对端已处理响应，应在应用协议中定义确认消息。
- `close()` 在所属 I/O 线程立即关闭连接，在其他线程提交关闭请求。主动关闭可能丢弃待发送数据。`armIdle(timeout)` 用于主动启用或重置空闲定时器，到期时立即关闭连接。
- `post(task)` 将状态更新投递到所属 I/O 线程，并返回任务是否被接收。在所属线程调用时，任务可以直接执行。已接收的任务可能在连接关闭后运行。任务需要操作仍打开的连接时，请在任务内检查 `isClosed()`。

I/O 回调应尽快返回。需要执行耗时工作时，先解析完整请求并保存所需数据，再交给应用自己的业务队列。

1. 在所属 I/O 线程调用 `beginWork()`，然后提交任务。
2. 将独立持有的请求数据和连接句柄副本交给业务线程。
3. 先用 `send()` 排队发送响应，再调用 `finishWork()`。每次 `beginWork()` 都应对应一次 `finishWork()`，提交失败、任务取消和出错时也要配对。

对端正常半关闭后，xnet 会在已登记的任务或待发数据处理完之前保留连接。帧长和业务队列上限由接入应用控制，示例中的行长限制就是一个例子。

## 有序停机

`run()` 会阻塞调用线程，每个服务端实例只能运行一次。正常停机返回 `0`，失败时返回非零值。`port()` 变为非零后，监听已就绪。如果地址不可用或端口被占用，可以查看启动错误和返回值。

在独立的管理线程调用 `stop()`。使用业务线程池时，可以调用 `server.stop(drain_workers, std::chrono::seconds(5))`，其中 `drain_workers` 等待 I/O 回调已提交的任务完成。

1. xnet 停止接入连接和派发接收事件。
2. xnet 在调用 `stop()` 的线程运行可选回调。此时 I/O 循环仍可处理响应和投递的任务。
3. 回调返回后，xnet 排空连接。连接进入排空阶段后拒绝新的发送和投递。发送排空超时从这一步开始计时，默认是五秒。
4. `stop()` 返回后，等待运行 `run()` 的线程退出，再销毁服务端。对同一服务端的生命周期调用应在停机回调之外进行。停机回调抛出异常时，`stop()` 会先完成清理，再将异常传给调用方。

## 使用底层缓冲区

自行管理套接字的应用可以引用 [`<xnet/network_buffer.hpp>`](../include/xnet/network_buffer.hpp)。使用 `append()` 追加来自独立存储区的字节，通过 `peek()` 和 `readable()` 查看可读数据，再用 `retrieve(n)` 消费数据，其中 `n <= readable()`。修改缓冲区可能使 `peek()` 返回的指针失效。`read_from_fd()` 把套接字收到的字节追加到缓冲区，返回读取字节数，EOF 时返回 `0`，出错时返回 `-1` 并设置 `errno`。使用非阻塞套接字时，`write_to_fd()` 会消费已成功写入的字节，剩余数据可在套接字重新可写时继续发送。通过 `TcpServer` 和 `Connection` 使用 xnet 时，传输缓冲区由库内部管理。
