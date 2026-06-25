#pragma once

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

namespace voris::io {

class async_semaphore {
    struct waiter_state {
        explicit waiter_state(async_semaphore* owner) noexcept
            : owner(owner) {}

        void complete(void_result value) {
            result = std::move(value);
            has_result = true;
        }

        async_semaphore* owner{nullptr};
        std::coroutine_handle<> continuation{};
        std::optional<scheduler_ref> scheduler{};
        void_result result{};
        bool has_result{false};
        bool enqueued{false};
        bool detached{false};
        bool observing{false};
    };

public:
    class acquire_operation {
    public:
        explicit acquire_operation(async_semaphore& semaphore)
            : state_(std::make_shared<waiter_state>(&semaphore)) {}

        acquire_operation(const acquire_operation&) = delete;
        acquire_operation& operator=(const acquire_operation&) = delete;

        acquire_operation(acquire_operation&& other) noexcept
            : state_(std::move(other.state_)) {}

        acquire_operation& operator=(acquire_operation&& other) noexcept {
            if (this != &other) {
                detach();
                state_ = std::move(other.state_);
            }
            return *this;
        }

        ~acquire_operation() {
            detach();
        }

        [[nodiscard]] bool await_ready() {
            if (!state_ || state_->owner == nullptr) {
                complete_invalid_state();
                return true;
            }
            return state_->owner->try_complete_immediate(state_);
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            if (!state_ || state_->owner == nullptr) {
                complete_invalid_state();
                return false;
            }

            auto scheduler = current_scheduler();
            if (!scheduler.has_value()) {
                state_->complete(std::unexpected(make_error(
                    vio_error_code::invalid_state, "semaphore acquire without a current scheduler")));
                state_->owner = nullptr;
                return false;
            }

            return state_->owner->enqueue_waiter(state_, continuation, *scheduler);
        }

        [[nodiscard]] void_result await_resume() {
            if (!state_) {
                return std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "empty semaphore acquire operation"));
            }

            state_->observing = true;
            state_->continuation = {};
            state_->scheduler.reset();
            state_->owner = nullptr;

            void_result result = state_->has_result
                                     ? std::move(state_->result)
                                     : std::unexpected(make_error(
                                           vio_error_code::invalid_state,
                                           "semaphore acquire resumed without completion"));
            state_.reset();
            return result;
        }

    private:
        void complete_invalid_state() {
            if (state_ && !state_->has_result) {
                state_->complete(std::unexpected(make_error(
                    vio_error_code::invalid_state, "invalid semaphore acquire operation")));
            }
        }

        void detach() noexcept {
            if (!state_ || state_->observing) {
                return;
            }

            state_->detached = true;
            state_->continuation = {};
            state_->scheduler.reset();
            if (state_->owner != nullptr) {
                state_->owner->detach_waiter(state_);
            }
            state_.reset();
        }

        std::shared_ptr<waiter_state> state_;
    };

    explicit async_semaphore(std::size_t permits)
        : async_semaphore(permits, default_max_waiters(permits)) {}

    async_semaphore(std::size_t permits, std::size_t max_waiters) noexcept
        : permits_(permits),
          max_waiters_(max_waiters) {}

    async_semaphore(const async_semaphore&) = delete;
    async_semaphore& operator=(const async_semaphore&) = delete;

    ~async_semaphore() {
        for (auto& waiter : waiters_) {
            if (waiter) {
                waiter->owner = nullptr;
                waiter->enqueued = false;
                waiter->detached = true;
                waiter->continuation = {};
                waiter->scheduler.reset();
            }
        }
    }

    [[nodiscard]] static constexpr std::size_t default_max_waiters(
        std::size_t initial_permits) noexcept {
        return initial_permits == 0 ? 1 : initial_permits;
    }

    [[nodiscard]] void_result try_acquire() {
        if (permits_ == 0) {
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "no semaphore permits available"));
        }

        --permits_;
        return {};
    }

    [[nodiscard]] acquire_operation acquire() {
        return acquire_operation(*this);
    }

    void release(std::size_t permits = 1) noexcept {
        if (permits == 0) {
            return;
        }

        std::size_t remaining = permits;
        while (remaining != 0 && !waiters_.empty()) {
            auto waiter = std::move(waiters_.front());
            waiters_.pop_front();
            if (!prepare_waiter_for_resume(waiter)) {
                continue;
            }

            --remaining;
            auto scheduler = *waiter->scheduler;
            waiter->complete({});
            waiter->owner = nullptr;
            waiter->enqueued = false;

            auto scheduled = trampoline::schedule_system(
                scheduler, [state = std::move(waiter), scheduler] mutable {
                    resume_waiter(std::move(state), scheduler);
                });
            if (!scheduled.has_value()) {
                std::terminate();
            }
        }

        add_permits_saturating(remaining);
    }

    [[nodiscard]] std::size_t permits() const noexcept {
        return permits_;
    }

    [[nodiscard]] std::size_t waiters() const noexcept {
        return waiters_.size();
    }

    [[nodiscard]] std::size_t max_waiters() const noexcept {
        return max_waiters_;
    }

private:
    [[nodiscard]] bool try_complete_immediate(const std::shared_ptr<waiter_state>& state) {
        if (!state || state->detached || state->has_result) {
            return true;
        }

        if (permits_ != 0) {
            --permits_;
            state->complete({});
            state->owner = nullptr;
            return true;
        }

        if (waiters_.size() >= max_waiters_) {
            state->complete(std::unexpected(make_error(vio_error_code::resource_exhausted,
                                                       "semaphore wait queue is full")));
            state->owner = nullptr;
            return true;
        }

        return false;
    }

    [[nodiscard]] bool enqueue_waiter(const std::shared_ptr<waiter_state>& state,
                                      std::coroutine_handle<> continuation,
                                      scheduler_ref scheduler) {
        if (try_complete_immediate(state)) {
            return false;
        }

        state->continuation = continuation;
        state->scheduler = scheduler;
        state->enqueued = true;
        waiters_.push_back(state);
        return true;
    }

    void detach_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
        if (!state) {
            return;
        }

        if (state->enqueued) {
            auto found = std::find_if(waiters_.begin(), waiters_.end(),
                                      [&state](const std::shared_ptr<waiter_state>& queued) {
                                          return queued.get() == state.get();
                                      });
            if (found != waiters_.end()) {
                waiters_.erase(found);
            }
            state->enqueued = false;
        }

        state->owner = nullptr;
    }

    [[nodiscard]] static bool prepare_waiter_for_resume(
        const std::shared_ptr<waiter_state>& waiter) noexcept {
        if (!waiter) {
            return false;
        }

        waiter->enqueued = false;
        if (waiter->detached || !waiter->continuation || !waiter->scheduler.has_value()) {
            waiter->owner = nullptr;
            return false;
        }

        return true;
    }

    static void resume_waiter(std::shared_ptr<waiter_state> state, scheduler_ref scheduler) {
        if (!state || state->detached || state->observing || !state->continuation) {
            return;
        }

        auto continuation = std::exchange(state->continuation, {});
        state->scheduler.reset();
        current_scheduler_scope scope(scheduler);
        continuation.resume();
    }

    void add_permits_saturating(std::size_t permits) noexcept {
        const auto max_permits = std::numeric_limits<std::size_t>::max();
        if (permits > max_permits - permits_) {
            permits_ = max_permits;
            return;
        }

        permits_ += permits;
    }

    std::size_t permits_;
    std::size_t max_waiters_;
    std::deque<std::shared_ptr<waiter_state>> waiters_;
};

} // namespace voris::io
