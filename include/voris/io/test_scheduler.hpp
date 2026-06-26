#pragma once

#include <cstddef>
#include <deque>
#include <utility>

#include <voris/io/scheduler.hpp>

namespace voris::io {

class test_scheduler {
public:
    using continuation = voris::io::continuation;

    test_scheduler() = default;

    test_scheduler(const test_scheduler&) = delete;
    test_scheduler& operator=(const test_scheduler&) = delete;

    test_scheduler(test_scheduler&&) = delete;
    test_scheduler& operator=(test_scheduler&&) = delete;

    ~test_scheduler() = default;

    void enqueue(continuation next) {
        ready_.push_back(std::move(next));
    }

    [[nodiscard]] bool empty() const noexcept {
        return ready_.empty();
    }

    [[nodiscard]] std::size_t ready_count() const noexcept {
        return ready_.size();
    }

    [[nodiscard]] bool run_one() {
        if (ready_.empty()) {
            return false;
        }

        continuation next = std::move(ready_.front());
        ready_.pop_front();
        if (next) {
            next();
        }
        return true;
    }

    [[nodiscard]] std::size_t run_ready() {
        const std::size_t ready_count = ready_.size();
        std::size_t ran = 0;
        while (ran < ready_count && run_one()) {
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

private:
    std::deque<continuation> ready_;
};

} // namespace voris::io
