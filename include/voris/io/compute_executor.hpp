#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class compute_executor {
public:
    explicit compute_executor(std::size_t queue_limit)
        : capacity_(queue_limit) {}

    [[nodiscard]] void_result submit(continuation work) {
        std::lock_guard lock(mutex_);
        if (shutting_down_) {
            return std::unexpected(make_error(vio_error_code::closed,
                                              "compute executor is shutting down"));
        }
        if (!has_capacity_locked()) {
            record_capacity_waiter_locked();
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "compute executor queue is full"));
        }
        queue_.push_back(std::move(work));
        capacity_waiters_ = 0;
        return {};
    }

    [[nodiscard]] void_result try_register_capacity_waiter() {
        std::lock_guard lock(mutex_);
        if (shutting_down_) {
            return std::unexpected(make_error(vio_error_code::closed,
                                              "compute executor is shutting down"));
        }
        if (has_capacity_locked()) {
            capacity_waiters_ = 0;
            return {};
        }
        record_capacity_waiter_locked();
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "compute executor capacity is unavailable"));
    }

    void release_capacity_waiter() {
        std::lock_guard lock(mutex_);
        if (capacity_waiters_ != 0) {
            --capacity_waiters_;
        }
    }

    [[nodiscard]] std::size_t run_until_idle() {
        std::size_t ran = 0;
        for (;;) {
            continuation work;
            {
                std::lock_guard lock(mutex_);
                if (queue_.empty()) {
                    break;
                }
                work = std::move(queue_.front());
                queue_.pop_front();
                capacity_waiters_ = 0;
            }
            if (work) {
                work();
            }
            ++ran;
        }
        return ran;
    }

    void request_shutdown() {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
        capacity_waiters_ = 0;
    }

    void shutdown() {
        request_shutdown();
    }

    [[nodiscard]] bool shutting_down() const {
        std::lock_guard lock(mutex_);
        return shutting_down_;
    }

    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t capacity_waiters() const {
        std::lock_guard lock(mutex_);
        return capacity_waiters_;
    }

private:
    [[nodiscard]] bool has_capacity_locked() const noexcept {
        return capacity_ != 0 && queue_.size() < capacity_;
    }

    void record_capacity_waiter_locked() noexcept {
        const std::size_t waiter_limit = capacity_ == 0 ? 1 : capacity_;
        if (capacity_waiters_ < waiter_limit) {
            ++capacity_waiters_;
        }
    }

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<continuation> queue_;
    std::size_t capacity_waiters_{0};
    bool shutting_down_{false};
};

} // namespace voris::io
