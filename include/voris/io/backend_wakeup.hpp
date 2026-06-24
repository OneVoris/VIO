#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace voris::io {

class backend_wakeup {
public:
    backend_wakeup() = default;

    backend_wakeup(const backend_wakeup&) = delete;
    backend_wakeup& operator=(const backend_wakeup&) = delete;

    void wake() {
        {
            std::lock_guard lock(mutex_);
            ++pending_;
        }
        condition_.notify_one();
    }

    [[nodiscard]] bool try_consume() {
        std::lock_guard lock(mutex_);
        if (pending_ == 0) {
            return false;
        }
        --pending_;
        return true;
    }

    [[nodiscard]] std::size_t consume_all() {
        std::lock_guard lock(mutex_);
        return std::exchange(pending_, 0);
    }

    void consume() {
        (void)try_consume();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        if (pending_ == 0) {
            ++waiters_;
            condition_.notify_all();
            condition_.wait(lock, [this] { return pending_ != 0; });
            --waiters_;
            condition_.notify_all();
        }
        --pending_;
    }

    template<class Rep, class Period>
    [[nodiscard]] bool wait_for(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock lock(mutex_);
        if (pending_ == 0) {
            ++waiters_;
            condition_.notify_all();
            const bool signaled =
                condition_.wait_for(lock, timeout, [this] { return pending_ != 0; });
            --waiters_;
            condition_.notify_all();
            if (!signaled) {
                return false;
            }
        }
        --pending_;
        return true;
    }

    [[nodiscard]] std::size_t pending_count() const {
        std::lock_guard lock(mutex_);
        return pending_;
    }

    [[nodiscard]] std::size_t wake_count() const {
        return pending_count();
    }

private:
    template<class Rep, class Period>
    [[nodiscard]] bool wait_for_waiter_count_for(
        std::size_t minimum_waiters,
        const std::chrono::duration<Rep, Period>& timeout) const {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this, minimum_waiters] {
                                       return waiters_ >= minimum_waiters;
                                   });
    }

    friend class shard;

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::size_t pending_{0};
    std::size_t waiters_{0};
};

} // namespace voris::io
