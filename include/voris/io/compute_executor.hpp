#pragma once

#include <cstddef>

#include <voris/io/detail/bounded_queue.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class compute_executor {
public:
    explicit compute_executor(std::size_t queue_limit)
        : queue_(queue_limit) {}

    [[nodiscard]] void_result submit(continuation work) {
        return queue_.try_push(std::move(work));
    }

    [[nodiscard]] std::size_t run_until_idle() {
        std::size_t ran = 0;
        while (auto work = queue_.pop()) {
            if (*work) {
                (*work)();
            }
            ++ran;
        }
        return ran;
    }

    [[nodiscard]] std::size_t queued() const {
        return queue_.size();
    }

    [[nodiscard]] std::size_t capacity_waiters() const {
        return queue_.capacity_waiters();
    }

private:
    detail::bounded_queue<continuation> queue_;
};

} // namespace voris::io
