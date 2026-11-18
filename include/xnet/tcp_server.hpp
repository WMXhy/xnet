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

    // Call from a management thread outside the server callbacks and hook.
    // Stop reads, invoke the hook with I/O loops running, then drain and
    // close all connections. The flush timeout starts after the hook.
    // Join the thread calling run() before destroying the server.
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
