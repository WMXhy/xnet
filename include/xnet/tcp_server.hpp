#pragma once

#include "xnet/connection.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xnet {

struct ServerOptions {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    std::size_t io_threads = 1;
    int backlog = 5;

    // 每条连接待发送字节的上限，包含跨线程提交。
    // 已写入套接字的字节会归还额度。设为零时只接收空发送。
    std::size_t max_pending_bytes = 4 * 1024 * 1024;
};

// Callbacks for a connection run on its owning I/O thread. on_open can
// override the default read, EOF and close callbacks for that connection.
struct ServerCallbacks {
    Connection::Callback on_open;
    Connection::BytesCallback on_bytes;
    Connection::Callback on_read_end;
    Connection::Callback on_close;
};

class TcpServer {
public:
    explicit TcpServer(ServerOptions options, ServerCallbacks callbacks = {});
    ~TcpServer() noexcept;

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Run once, blocking the caller. A startup failure returns 1.
    int run();

    // 在服务端回调和停机钩子以外的管理线程调用。
    // 停止读取后保持 I/O 循环运行，允许停机钩子发送和投递任务。
    // 钩子结束后逐个排空并关闭连接。连接进入最终排空时停止接收新提交。
    // 排空超时从钩子结束后开始计时。销毁服务端前等待 run() 所在线程退出。
    void stop(std::function<void()> after_quiesce = {},
              std::chrono::milliseconds flush_timeout = std::chrono::seconds(5));

    // The bound port when running. Use port 0 in ServerOptions for an
    // ephemeral port. Returns 0 before startup or when stopping.
    std::uint16_t port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xnet
