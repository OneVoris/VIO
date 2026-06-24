#pragma once

#include <voris/io/detail/bounded_queue.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io::detail {

class mailbox {
public:
    explicit mailbox(std::size_t capacity)
        : queue_(capacity) {}

    [[nodiscard]] void_result submit(continuation message) {
        return queue_.try_push(std::move(message));
    }

    [[nodiscard]] bool run_one() {
        auto next = queue_.pop();
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
        return queue_.size();
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
    bounded_queue<continuation> queue_;
};

} // namespace voris::io::detail
