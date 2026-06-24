#pragma once

#include <voris/io/detail/bounded_queue.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io::detail {

class mailbox {
public:
    explicit mailbox(std::size_t capacity)
        : queue_(capacity),
          system_queue_(system_capacity_for(capacity)) {}

    [[nodiscard]] void_result submit(continuation message) {
        return queue_.try_push(std::move(message));
    }

    [[nodiscard]] void_result submit_system(continuation message) {
        return system_queue_.try_push(std::move(message));
    }

    [[nodiscard]] bool run_one() {
        auto next = system_queue_.pop();
        if (!next.has_value()) {
            next = queue_.pop();
        }
        if (!next.has_value()) {
            return false;
        }
        if (*next) {
            (*next)();
        }
        return true;
    }

    [[nodiscard]] std::size_t run_until_idle() {
        std::size_t ran = 0;
        while (run_one()) {
            ++ran;
        }
        return ran;
    }

    [[nodiscard]] std::size_t size() const {
        return queue_.size() + system_queue_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return queue_.capacity();
    }

    [[nodiscard]] bool full() const {
        return queue_.full();
    }

    // Saturated count of failed submits observed while the mailbox is full.
    [[nodiscard]] std::size_t full_pressure() const {
        return queue_.full_pressure();
    }

    [[nodiscard]] std::size_t capacity_waiters() const {
        return queue_.capacity_waiters();
    }

private:
    static constexpr std::size_t system_capacity_for(std::size_t user_capacity) noexcept {
        return user_capacity;
    }

    bounded_queue<continuation> queue_;
    bounded_queue<continuation> system_queue_;
};

} // namespace voris::io::detail
