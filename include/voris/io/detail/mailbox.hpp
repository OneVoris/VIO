#pragma once

#include <chrono>
#include <optional>
#include <utility>

#include <voris/io/detail/bounded_queue.hpp>
#include <voris/io/loop_budget.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class shard;

} // namespace voris::io

namespace voris::io::detail {

class mailbox {
public:
    using clock = std::chrono::steady_clock;

    struct message {
        continuation work;
        clock::time_point enqueued_at;
    };

    explicit mailbox(std::size_t capacity)
        : queue_(capacity),
          system_queue_(system_capacity_for(capacity)) {}

    [[nodiscard]] void_result submit(continuation message) {
        return queue_.try_push(make_message(std::move(message)));
    }

    [[nodiscard]] void_result submit_system(continuation message) {
        return system_queue_.try_push(make_message(std::move(message)));
    }

    [[nodiscard]] bool run_one() {
        auto next = pop_next();
        if (!next.has_value()) {
            return false;
        }
        run(*next);
        return true;
    }

    [[nodiscard]] std::size_t run_for_budget(loop_budget_slice& budget) {
        std::size_t ran = 0;
        while (budget.remaining_tasks() > 0) {
            auto next = pop_next();
            if (!next.has_value()) {
                break;
            }
            (void)budget.consume_task();
            run(*next);
            ++ran;
        }
        return ran;
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
    friend class ::voris::io::shard;

    // Transfers unexecuted work to the shard so it can publish instrumentation before
    // running user code; callers must not execute the work while holding internal locks.
    [[nodiscard]] std::optional<message> pop_next() {
        auto next = system_queue_.pop();
        if (!next.has_value()) {
            next = queue_.pop();
        }
        return next;
    }

    [[nodiscard]] static message make_message(continuation work) {
        return message{.work = std::move(work), .enqueued_at = clock::now()};
    }

    static void run(message& message) {
        if (message.work) {
            message.work();
        }
    }

    static constexpr std::size_t system_capacity_for(std::size_t user_capacity) noexcept {
        return user_capacity;
    }

    bounded_queue<message> queue_;
    bounded_queue<message> system_queue_;
};

} // namespace voris::io::detail
