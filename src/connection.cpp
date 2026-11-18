#include <xnet/connection.hpp>
#include <xnet/network_buffer.hpp>

#include "event_loop.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <optional>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

namespace xnet {

struct Connection::Impl {
    Impl(int descriptor, const std::shared_ptr<detail::EventLoop>& owner, std::uint64_t connection_id)
        : fd(descriptor), loop(owner), id(connection_id) {}

    ~Impl() {
        if (fd >= 0) ::close(fd);
    }

    int fd;
    const std::weak_ptr<detail::EventLoop> loop;
    const std::uint64_t id;

    NetworkBuffer reader;
    NetworkBuffer writer;
    std::shared_ptr<BytesCallback> on_read;
    std::shared_ptr<Callback> on_read_end;
    Callback on_close;
    Callback on_transport_close;

    std::atomic<bool> closed{false};
    bool read_eof = false;
    bool read_paused = false;
    bool draining = false;
    std::size_t pending_work = 0;
    std::optional<detail::EventLoop::TimerId> idle_timer;
    std::optional<detail::EventLoop::TimerId> close_timer;
};

Connection::Connection(int fd, std::shared_ptr<detail::EventLoop> loop, std::uint64_t id)
    : impl_(std::make_unique<Impl>(fd, loop, id)) {}

Connection::~Connection() = default;

std::uint64_t Connection::id() const noexcept {
    return impl_->id;
}

bool Connection::isClosed() const noexcept {
    return impl_->closed.load();
}

void Connection::setReadCallback(BytesCallback callback) {
    if (isClosed()) return;
    impl_->on_read = callback ? std::make_shared<BytesCallback>(std::move(callback)) : nullptr;
}

void Connection::setReadEndCallback(Callback callback) {
    if (isClosed()) return;
    impl_->on_read_end = callback ? std::make_shared<Callback>(std::move(callback)) : nullptr;
}

void Connection::setCloseCallback(Callback callback) {
    if (!isClosed()) impl_->on_close = std::move(callback);
}

void Connection::setTransportCloseCallback(Callback callback) {
    impl_->on_transport_close = std::move(callback);
}

bool Connection::post(std::function<void()> task) {
    if (isClosed()) return false;
    const auto loop = impl_->loop.lock();
    if (!loop) return false;

    return loop->runInLoop([self = shared_from_this(), task = std::move(task)] {
        try {
            task();
        } catch (...) {
            self->handleClose();
        }
    });
}

void Connection::send(std::vector<char> bytes) {
    if (bytes.empty()) return;

    post([self = shared_from_this(), bytes = std::move(bytes)] {
        if (self->isClosed()) return;
        self->impl_->writer.append(bytes.data(), bytes.size());
        self->updateEvents();
    });
}

void Connection::close() {
    if (isClosed()) return;
    const auto loop = impl_->loop.lock();
    if (!loop) return;

    if (loop->isInLoopThread()) {
        handleClose();
    } else {
        loop->runInLoop([self = shared_from_this()] { self->handleClose(); });
    }
}

void Connection::armIdle(Duration timeout) {
    post([self = shared_from_this(), timeout] {
        if (self->isClosed()) return;
        auto& state = *self->impl_;
        const auto loop = state.loop.lock();
        if (!loop) return;

        if (state.idle_timer) loop->cancelTimer(*state.idle_timer);
        const std::weak_ptr<Connection> weak_self = self;
        state.idle_timer = loop->runAfter(timeout, [weak_self] {
            if (const auto connection = weak_self.lock()) connection->handleClose();
        });
    });
}

void Connection::beginWork() {
    assert(!isClosed());
    ++impl_->pending_work;
}

void Connection::finishWork() {
    post([self = shared_from_this()] {
        if (self->isClosed()) return;
        auto& remaining = self->impl_->pending_work;
        assert(remaining > 0);
        --remaining;
        self->maybeClose();
    });
}

void Connection::handleEvents(std::uint32_t events) {
    if (isClosed()) return;

    try {
        if (events & EPOLLERR) {
            handleClose();
            return;
        }
        if (!impl_->read_paused && !impl_->read_eof && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) {
            handleRead();
        }
        if (!isClosed() && (events & EPOLLOUT)) handleWrite();
    } catch (...) {
        handleClose();
    }
}

void Connection::handleRead() {
    bool received_eof = false;
    auto& state = *impl_;
    while (true) {
        const auto count = state.reader.read_from_fd(state.fd);
        if (count > 0) continue;
        if (count == 0) {
            received_eof = true;
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;

        handleClose();
        return;
    }

    const auto size = state.reader.readable();
    if (size > 0) {
        if (const auto callback = state.on_read) {
            (*callback)(shared_from_this(), std::string_view(state.reader.peek(), size));
        }
        state.reader.retrieve(size);
        if (isClosed()) return;
    }

    if (received_eof) {
        state.read_eof = true;
        if (const auto callback = state.on_read_end) (*callback)(shared_from_this());
        updateEvents();
        maybeClose();
    }
}

void Connection::handleWrite() {
    auto& state = *impl_;
    if (state.writer.write_to_fd(state.fd) < 0) {
        handleClose();
        return;
    }
    updateEvents();
    maybeClose();
}

void Connection::handleClose() {
    auto& state = *impl_;
    if (state.closed.exchange(true)) return;

    const auto self = shared_from_this();
    if (const auto loop = state.loop.lock()) {
        if (state.idle_timer) loop->cancelTimer(*state.idle_timer);
        if (state.close_timer) loop->cancelTimer(*state.close_timer);
        loop->delEvent(state.fd);
    }
    ::close(std::exchange(state.fd, -1));

    // Release per-connection callbacks even when callers retain the closed handle.
    state.on_read.reset();
    state.on_read_end.reset();
    auto on_close = std::move(state.on_close);
    auto on_transport_close = std::move(state.on_transport_close);
    if (on_close) {
        try {
            on_close(self);
        } catch (...) {
            // The server must still remove the connection and finish draining.
        }
    }
    if (on_transport_close) on_transport_close(self);
}

void Connection::updateEvents() {
    if (isClosed()) return;
    const auto& state = *impl_;
    const auto loop = state.loop.lock();
    if (!loop) return;

    std::uint32_t events = EPOLLET;
    if (!state.read_eof && !state.read_paused) events |= EPOLLIN | EPOLLRDHUP;
    if (state.writer.readable() > 0) events |= EPOLLOUT;
    loop->modEvent(state.fd, events);
}

void Connection::pauseRead() {
    if (isClosed() || impl_->read_paused) return;

    impl_->read_paused = true;
    try {
        updateEvents();
    } catch (...) {
        handleClose();
    }
}

void Connection::maybeClose() {
    const auto& state = *impl_;
    if ((state.read_eof || state.draining) && state.pending_work == 0 && state.writer.readable() == 0) {
        handleClose();
    }
}

void Connection::closeAfterFlush(Duration timeout) {
    if (isClosed() || impl_->draining) return;

    impl_->draining = true;
    pauseRead();
    maybeClose();
    if (isClosed()) return;

    const auto loop = impl_->loop.lock();
    if (!loop) return;

    try {
        const std::weak_ptr<Connection> weak_self = shared_from_this();
        impl_->close_timer = loop->runAfter(timeout, [weak_self] {
            if (const auto connection = weak_self.lock()) connection->handleClose();
        });
    } catch (...) {
        handleClose();
    }
}

} // namespace xnet
