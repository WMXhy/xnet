#include "xnet/tcp_server.hpp"

#include "acceptor.hpp"
#include "event_loop.hpp"

#include <sys/epoll.h>

#include <atomic>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xnet {
namespace {

using detail::Acceptor;
using detail::EventLoop;
using detail::SocketFd;

class ServerThreadScope {
public:
    explicit ServerThreadScope(const void* server)
        : server_(server), previous_(std::exchange(current_, this)) {}
    ~ServerThreadScope() { current_ = previous_; }

    static bool contains(const void* server) noexcept {
        for (auto* scope = current_; scope; scope = scope->previous_) {
            if (scope->server_ == server) return true;
        }
        return false;
    }

private:
    static thread_local const ServerThreadScope* current_;

    const void* server_;
    const ServerThreadScope* previous_;
};

thread_local const ServerThreadScope* ServerThreadScope::current_ = nullptr;

bool inLoopAndWait(EventLoop& loop, std::function<void()> operation) {
    auto task = std::make_shared<std::packaged_task<void()>>(std::move(operation));
    auto done = task->get_future();
    if (!loop.runInLoop([task] { (*task)(); })) return false;
    done.get();
    return true;
}

void reportError(const char* operation) noexcept {
    try {
        throw;
    } catch (const std::exception& error) {
        std::cerr << "xnet " << operation << ": " << error.what() << '\n';
    } catch (...) {
        std::cerr << "xnet " << operation << ": unknown exception\n";
    }
}

} // namespace

class TcpServer::Impl {
public:
    Impl(ServerOptions options, ServerCallbacks callbacks)
        : options_(std::move(options)), callbacks_(std::move(callbacks)) {
        if (options_.io_threads == 0) throw std::invalid_argument("xnet requires an I/O thread");
        if (options_.backlog <= 0) throw std::invalid_argument("xnet backlog must be positive");
    }

    int run();
    void stop(std::function<void()> after_quiesce, std::chrono::milliseconds flush_timeout);
    std::uint16_t port() const noexcept { return port_.load(); }

private:
    struct Worker {
        std::shared_ptr<EventLoop> loop;
        std::thread thread;
        std::unordered_map<std::uint64_t, Connection::Ptr> connections;
        std::shared_ptr<std::promise<void>> drained;

        std::vector<Connection::Ptr> snapshot() const {
            std::vector<Connection::Ptr> result;
            result.reserve(connections.size());
            for (const auto& entry : connections) result.push_back(entry.second);
            return result;
        }

        void notifyIfDrained() {
            if (connections.empty() && drained) {
                auto done = std::move(drained);
                done->set_value();
            }
        }
    };

    enum class State { Created, Running, Stopped };

    void startWorkers();
    void stopWorkers();
    void scheduleConnection(SocketFd socket) noexcept;
    void attachConnection(Worker& worker, SocketFd& socket, std::uint64_t id) noexcept;
    void drainConnections(std::chrono::milliseconds timeout);

    const ServerOptions options_;
    const ServerCallbacks callbacks_;
    std::mutex lifecycle_mutex_;
    State state_ = State::Created;
    std::atomic<std::uint16_t> port_{0};
    std::shared_ptr<EventLoop> listener_loop_;
    std::unique_ptr<Acceptor> listener_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::size_t next_worker_ = 0;
    std::uint64_t next_connection_id_ = 1;
};

TcpServer::TcpServer(ServerOptions options, ServerCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(callbacks))) {}

TcpServer::~TcpServer() noexcept {
    stop();
}

int TcpServer::run() {
    return impl_->run();
}

void TcpServer::stop(std::function<void()> after_quiesce, std::chrono::milliseconds flush_timeout) {
    impl_->stop(std::move(after_quiesce), flush_timeout);
}

std::uint16_t TcpServer::port() const noexcept {
    return impl_->port();
}

int TcpServer::Impl::run() {
    if (ServerThreadScope::contains(this)) return 1;

    std::shared_ptr<EventLoop> loop;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (state_ != State::Created) return 1;

        try {
            loop = std::make_shared<EventLoop>();
            auto listener = std::make_unique<Acceptor>(
                *loop, options_.address, options_.port, options_.backlog,
                [this](SocketFd socket) { scheduleConnection(std::move(socket)); });
            startWorkers();
            listener_loop_ = loop;
            listener_ = std::move(listener);
            state_ = State::Running;
            port_.store(listener_->port());
        } catch (...) {
            stopWorkers();
            state_ = State::Stopped;
            reportError("startup failed");
            return 1;
        }
    }

    try {
        ServerThreadScope scope(this);
        loop->loop();
    } catch (...) {
        reportError("listener loop failed");
        stop({}, std::chrono::seconds(5));
        return 1;
    }
    return 0;
}

void TcpServer::Impl::startWorkers() {
    workers_.reserve(options_.io_threads);
    for (std::size_t i = 0; i < options_.io_threads; ++i) {
        auto& worker = *workers_.emplace_back(std::make_unique<Worker>());
        std::promise<std::shared_ptr<EventLoop>> ready;
        auto started = ready.get_future();
        worker.thread = std::thread([this, &worker, ready = std::move(ready)]() mutable {
            ServerThreadScope scope(this);
            std::shared_ptr<EventLoop> loop;
            try {
                loop = std::make_shared<EventLoop>();
            } catch (...) {
                ready.set_exception(std::current_exception());
                return;
            }
            ready.set_value(loop);

            try {
                loop->loop();
            } catch (...) {
                reportError("I/O loop failed");
                while (!worker.connections.empty()) {
                    auto connection = worker.connections.begin()->second;
                    connection->close();
                }
            }
        });
        worker.loop = started.get();
    }
}

void TcpServer::Impl::stopWorkers() {
    for (const auto& worker : workers_) {
        if (worker->loop) worker->loop->quit();
    }
    for (const auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    workers_.clear();
}

void TcpServer::Impl::scheduleConnection(SocketFd socket) noexcept {
    try {
        auto& worker = *workers_[next_worker_];
        next_worker_ = (next_worker_ + 1) % workers_.size();

        const auto id = next_connection_id_++;
        auto accepted = std::make_shared<SocketFd>(std::move(socket));
        worker.loop->runInLoop([this, &worker, accepted, id] {
            attachConnection(worker, *accepted, id);
        });
    } catch (...) {
        reportError("connection dispatch failed");
    }
}

void TcpServer::Impl::attachConnection(Worker& worker, SocketFd& socket, std::uint64_t id) noexcept {
    Connection::Ptr connection;
    try {
        const int fd = socket.get();
        auto* owned = new Connection(fd, worker.loop, id, options_.max_pending_bytes);
        socket.release();
        connection.reset(owned);

        connection->setTransportCloseCallback([&worker](const Connection::Ptr& closed) {
            worker.connections.erase(closed->id());
            worker.notifyIfDrained();
        });
        connection->setReadCallback(callbacks_.on_bytes);
        connection->setReadEndCallback(callbacks_.on_read_end);
        connection->setCloseCallback(callbacks_.on_close);
        worker.connections.emplace(id, connection);
        worker.loop->addEvent(fd, EPOLLIN | EPOLLRDHUP | EPOLLET, [connection](std::uint32_t events) {
            connection->handleEvents(events);
        });

        if (callbacks_.on_open) callbacks_.on_open(connection);
    } catch (...) {
        reportError("connection setup failed");
        if (connection) connection->close();
    }
}

void TcpServer::Impl::drainConnections(std::chrono::milliseconds timeout) {
    std::vector<std::future<void>> finished;
    finished.reserve(workers_.size());
    for (const auto& worker : workers_) {
        auto drained = std::make_shared<std::promise<void>>();
        auto done = drained->get_future();
        const bool scheduled = inLoopAndWait(*worker->loop, [&worker = *worker, drained, timeout] {
            const auto connections = worker.snapshot();
            worker.drained = drained;
            for (const auto& connection : connections) connection->closeAfterFlush(timeout);
            worker.notifyIfDrained();
        });
        if (scheduled) finished.push_back(std::move(done));
    }
    for (auto& done : finished) done.get();
}

void TcpServer::Impl::stop(std::function<void()> after_quiesce, std::chrono::milliseconds flush_timeout) {
    if (ServerThreadScope::contains(this)) {
        throw std::logic_error("Stop xnet outside its callbacks and shutdown hook");
    }
    ServerThreadScope scope(this);

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ == State::Stopped) return;
    if (state_ == State::Created) {
        state_ = State::Stopped;
        if (after_quiesce) after_quiesce();
        return;
    }

    port_.store(0);
    if (!inLoopAndWait(*listener_loop_, [this] { listener_.reset(); })) {
        listener_.reset();
    }
    for (const auto& worker : workers_) {
        inLoopAndWait(*worker->loop, [&worker = *worker] {
            for (const auto& connection : worker.snapshot()) connection->pauseRead();
        });
    }

    std::exception_ptr hook_error;
    try {
        if (after_quiesce) after_quiesce();
    } catch (...) {
        hook_error = std::current_exception();
    }

    drainConnections(flush_timeout);
    stopWorkers();
    listener_loop_->quit();
    listener_loop_.reset();
    state_ = State::Stopped;

    if (hook_error) std::rethrow_exception(hook_error);
}

} // namespace xnet
