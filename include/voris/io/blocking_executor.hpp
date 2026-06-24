#pragma once

#include <cstddef>

#include <voris/io/detail/bounded_queue.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class blocking_executor {
public:
    explicit blocking_executor(std::size_t queue_limit)
        : queue_(queue_limit) {}

    [[nodiscard]] void_result submit(continuation work) {
        return queue_.try_push(std::move(work));
    }

    [[nodiscard]] std::size_t drain() {
        std::size_t ran = 0;
        while (auto work = queue_.pop()) {
            if (*work) {
                (*work)();
            }
            ++ran;
        }
        return ran;
    }

    void shutdown() noexcept {
        shutting_down_ = true;
    }

    [[nodiscard]] bool shutting_down() const noexcept {
        return shutting_down_;
    }

    [[nodiscard]] std::size_t queued() const {
        return queue_.size();
    }

private:
    detail::bounded_queue<continuation> queue_;
    bool shutting_down_{false};
};

} // namespace voris::io
