#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>

namespace xnet::detail {

class EventLoop {
public:
    using Task = std::function<void()>;
    using Callback = std::function<void(std::uint32_t)>;
    using Duration = std::chrono::milliseconds;
    using TimerId = std::uint64_t;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Cross-thread. Tasks accepted before quit are run on the owning thread.
    bool runInLoop(Task task);
    void quit();
    bool isInLoopThread() const noexcept;

    // The following operations run on the owning thread.
    void addEvent(int fd, std::uint32_t flags, Callback callback);
    void modEvent(int fd, std::uint32_t flags);
    void delEvent(int fd) noexcept;
    TimerId runAfter(Duration delay, Task callback);
    void cancelTimer(TimerId id) noexcept;
    void loop();

private:
    using Clock = std::chrono::steady_clock;
    using TimerKey = std::pair<Clock::time_point, TimerId>;

    struct Registration {
        std::uint64_t token;
        std::shared_ptr<Callback> callback;
    };

    void closeDescriptors() noexcept;
    void wakeUp() noexcept;
    void runPending();
    void handleWakeup();
    void handleTimers();
    void resetTimer() noexcept;

    int epoll_fd_ = -1;
    int wakeup_fd_ = -1;
    int timer_fd_ = -1;
    const std::thread::id owner_;

    std::mutex mutex_;
    std::atomic<bool> stopping_{false};
    std::queue<Task> pending_;

    std::unordered_map<int, Registration> callbacks_;
    std::uint32_t next_registration_ = 0;

    std::map<TimerKey, Task> timers_;
    TimerId next_timer_ = 0;
};

} // namespace xnet::detail
