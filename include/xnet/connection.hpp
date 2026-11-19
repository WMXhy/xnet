#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace xnet {

namespace detail {
class EventLoop;
}
class TcpServer;

enum class SendResult {
    Queued,
    Closed,
    QueueFull,
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Ptr = std::shared_ptr<Connection>;
    using Callback = std::function<void(const Ptr&)>;
    using BytesCallback = std::function<void(const Ptr&, std::string_view)>;
    using Duration = std::chrono::milliseconds;

    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::uint64_t id() const noexcept;
    bool isClosed() const noexcept;

    // Set from the owning I/O thread. A read view is valid only within its callback.
    void setReadCallback(BytesCallback callback);
    void setReadEndCallback(Callback callback);
    void setCloseCallback(Callback callback);

    // 以下操作可跨线程调用。Queued 表示发送已入队。
    // 套接字出错或排空超时可能丢弃队列中的字节。进入最终排空后返回 Closed。
    SendResult send(std::vector<char> bytes);
    void close();
    void armIdle(Duration timeout);

    // 已接收的任务会执行，即使连接先关闭。进入最终排空后拒绝新任务。
    bool post(std::function<void()> task);

    // Call beginWork on the I/O thread, and finishWork after queuing any response.
    void beginWork();
    void finishWork();

private:
    friend class TcpServer;

    // Owns the nonblocking descriptor after successful construction.
    Connection(int fd, std::shared_ptr<detail::EventLoop> loop, std::uint64_t id,
               std::size_t max_pending_bytes);

    void setTransportCloseCallback(Callback callback);
    void handleEvents(std::uint32_t events);
    void pauseRead();
    void closeAfterFlush(Duration timeout);
    void handleRead();
    void handleWrite();
    void refreshWriteInterest();
    void handleClose();
    void updateEvents();
    void maybeClose();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xnet
