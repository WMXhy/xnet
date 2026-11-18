#include "event_loop.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace xnet::detail {
namespace {

template <typename Callback, typename... Args>
void invoke(Callback& callback, Args... args) noexcept {
    try {
        callback(args...);
    } catch (...) {
        // An application callback cannot stop service for other connections.
    }
}

void consumeCounter(int fd) noexcept {
    std::uint64_t count;
    ssize_t result;
    do {
        result = ::read(fd, &count, sizeof(count));
    } while (result < 0 && errno == EINTR);
}

} // namespace

EventLoop::EventLoop() : owner_(std::this_thread::get_id()) {
    try {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");

        wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) throw std::system_error(errno, std::generic_category(), "eventfd");

        timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd_ < 0) throw std::system_error(errno, std::generic_category(), "timerfd_create");

        addEvent(wakeup_fd_, EPOLLIN, [this](std::uint32_t) { handleWakeup(); });
        addEvent(timer_fd_, EPOLLIN, [this](std::uint32_t) { handleTimers(); });
    } catch (...) {
        closeDescriptors();
        throw;
    }
}

EventLoop::~EventLoop() {
    closeDescriptors();
}

void EventLoop::closeDescriptors() noexcept {
    if (timer_fd_ >= 0) ::close(timer_fd_);
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

bool EventLoop::isInLoopThread() const noexcept {
    return std::this_thread::get_id() == owner_;
}

bool EventLoop::runInLoop(Task task) {
    if (isInLoopThread()) {
        if (stopping_.load()) return false;
        task();
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load()) return false;
        pending_.push(std::move(task));
    }
    wakeUp();
    return true;
}

void EventLoop::quit() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true);
    }
    wakeUp();
}

void EventLoop::wakeUp() noexcept {
    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = ::write(wakeup_fd_, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

void EventLoop::runPending() {
    std::queue<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks.swap(pending_);
    }
    while (!tasks.empty()) {
        invoke(tasks.front());
        tasks.pop();
    }
}

void EventLoop::handleWakeup() {
    consumeCounter(wakeup_fd_);
    runPending();
}

void EventLoop::addEvent(int fd, std::uint32_t flags, Callback callback) {
    assert(isInLoopThread());
    const auto generation = static_cast<std::uint64_t>(next_registration_++);
    const auto token = (generation << 32) | static_cast<std::uint32_t>(fd);
    const auto result = callbacks_.emplace(
        fd, Registration{token, std::make_shared<Callback>(std::move(callback))});
    if (!result.second) throw std::logic_error("file descriptor already registered with xnet");

    epoll_event event{};
    event.events = flags;
    event.data.u64 = token;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
        const int error = errno;
        callbacks_.erase(result.first);
        throw std::system_error(error, std::generic_category(), "epoll_ctl add");
    }
}

void EventLoop::modEvent(int fd, std::uint32_t flags) {
    assert(isInLoopThread());
    const auto registration = callbacks_.find(fd);
    if (registration == callbacks_.end()) return;

    epoll_event event{};
    event.events = flags;
    event.data.u64 = registration->second.token;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl modify");
    }
}

void EventLoop::delEvent(int fd) noexcept {
    assert(isInLoopThread());
    if (callbacks_.erase(fd) != 0) ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void EventLoop::loop() {
    assert(isInLoopThread());

    int error = 0;
    std::array<epoll_event, 128> events;
    while (!stopping_.load()) {
        const int count = ::epoll_wait(epoll_fd_, events.data(), events.size(), -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            error = errno;
            quit();
            break;
        }

        for (int i = 0; i < count && !stopping_.load(); ++i) {
            const auto token = events[i].data.u64;
            const auto fd = static_cast<int>(static_cast<std::uint32_t>(token));
            const auto registration = callbacks_.find(fd);
            if (registration == callbacks_.end() || registration->second.token != token) continue;

            // The callback may remove its own registration and close the descriptor.
            const auto callback = registration->second.callback;
            invoke(*callback, events[i].events);
        }
    }
    runPending();

    if (error != 0) throw std::system_error(error, std::generic_category(), "epoll_wait");
}

EventLoop::TimerId EventLoop::runAfter(Duration delay, Task callback) {
    assert(isInLoopThread());
    const auto now = Clock::now();
    const auto max_delay = std::chrono::duration_cast<Duration>(Clock::time_point::max() - now);
    const auto when = delay >= max_delay ? Clock::time_point::max()
                                       : now + std::max(delay, Duration::zero());
    const auto id = next_timer_++;
    const auto entry = timers_.emplace(TimerKey{when, id}, std::move(callback));
    if (entry.first == timers_.begin()) resetTimer();
    return id;
}

void EventLoop::cancelTimer(TimerId id) noexcept {
    assert(isInLoopThread());
    for (auto entry = timers_.begin(); entry != timers_.end(); ++entry) {
        if (entry->first.second == id) {
            const bool earliest = entry == timers_.begin();
            timers_.erase(entry);
            if (earliest) resetTimer();
            return;
        }
    }
}

void EventLoop::handleTimers() {
    consumeCounter(timer_fd_);

    const auto now = Clock::now();
    const auto first_new_id = next_timer_;
    while (!timers_.empty()) {
        const auto entry = timers_.begin();
        const auto& key = entry->first;
        if (key.first > now || key.second >= first_new_id) break;

        auto callback = std::move(entry->second);
        timers_.erase(entry);
        invoke(callback);
    }
    resetTimer();
}

void EventLoop::resetTimer() noexcept {
    itimerspec spec{};
    if (!timers_.empty()) {
        const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
            timers_.begin()->first.first - Clock::now());
        const auto next = std::max(delay, std::chrono::nanoseconds{1});
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(next);
        spec.it_value.tv_sec = static_cast<time_t>(seconds.count());
        spec.it_value.tv_nsec = static_cast<long>((next - seconds).count());
    }

    int status;
    do {
        status = ::timerfd_settime(timer_fd_, 0, &spec, nullptr);
    } while (status < 0 && errno == EINTR);
}

} // namespace xnet::detail
