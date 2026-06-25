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
    struct waiter_state;

    struct semaphore_state {
        semaphore_state(std::size_t initial_permits, std::size_t waiter_limit) noexcept
            : permits(initial_permits),
              max_waiters(waiter_limit) {}

        std::size_t permits;
        std::size_t max_waiters;
        std::deque<std::shared_ptr<waiter_state>> waiters;
        bool alive{true};
    };

    struct waiter_state {
        explicit waiter_state(const std::shared_ptr<semaphore_state>& owner_state) noexcept
            : owner(owner_state) {}

        bool complete(void_result value) {
            if (has_result) {
                return false;
            }

            result = std::move(value);
            has_result = true;
            return true;
        }

        std::weak_ptr<semaphore_state> owner;
        std::coroutine_handle<> continuation{};
        std::optional<scheduler_ref> scheduler{};
        void_result result{};
        bool has_result{false};
        bool enqueued{false};
        bool detached{false};
        bool observing{false};
        bool permit_reserved{false};
    };

public:
    class acquire_operation {
    public:
        explicit acquire_operation(const std::shared_ptr<semaphore_state>& owner)
            : state_(std::make_shared<waiter_state>(owner)) {}

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
            return async_semaphore::try_complete_immediate(state_);
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            auto scheduler = current_scheduler();
            if (!scheduler.has_value()) {
                complete_invalid_state("semaphore acquire without a current scheduler");
                return false;
            }

            return async_semaphore::enqueue_waiter(state_, continuation, *scheduler);
        }

        [[nodiscard]] void_result await_resume() {
            if (!state_) {
                return std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "empty semaphore acquire operation"));
            }

            state_->observing = true;
            state_->continuation = {};
            state_->scheduler.reset();
            state_->owner.reset();
            state_->permit_reserved = false;

            void_result result = state_->has_result
                                     ? std::move(state_->result)
                                     : std::unexpected(make_error(
                                           vio_error_code::invalid_state,
                                           "semaphore acquire resumed without completion"));
            state_.reset();
            return result;
        }

    private:
        void complete_invalid_state(const char* diagnostic) {
            if (state_) {
                (void)state_->complete(
                    std::unexpected(make_error(vio_error_code::invalid_state, diagnostic)));
                state_->owner.reset();
            }
        }

        void detach() noexcept {
            if (!state_ || state_->observing) {
                return;
            }

            state_->detached = true;
            state_->continuation = {};
            state_->scheduler.reset();
            if (auto owner = state_->owner.lock()) {
                async_semaphore::detach_waiter(std::move(owner), state_);
            } else {
                state_->owner.reset();
            }
            state_.reset();
        }

        std::shared_ptr<waiter_state> state_;
    };

    // The single-argument constructor derives a strict waiter bound from the
    // initial permit count. A zero initial count still allows one waiter.
    explicit async_semaphore(std::size_t permits)
        : async_semaphore(permits, default_max_waiters(permits)) {}

    // max_waiters == 0 creates a non-queuing semaphore: acquire() can only
    // complete immediately from an available permit, otherwise it reports
    // resource_exhausted without adding a waiter.
    async_semaphore(std::size_t permits, std::size_t max_waiters)
        : state_(std::make_shared<semaphore_state>(permits, max_waiters)) {}

    async_semaphore(const async_semaphore&) = delete;
    async_semaphore& operator=(const async_semaphore&) = delete;

    ~async_semaphore() {
        close_state(std::move(state_));
    }

    [[nodiscard]] static constexpr std::size_t default_max_waiters(
        std::size_t initial_permits) noexcept {
        return initial_permits == 0 ? 1 : initial_permits;
    }

    [[nodiscard]] void_result try_acquire() {
        if (!state_ || !state_->alive) {
            return std::unexpected(make_error(vio_error_code::invalid_state,
                                              "semaphore is not alive"));
        }
        if (state_->permits == 0) {
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "no semaphore permits available"));
        }

        --state_->permits;
        return {};
    }

    [[nodiscard]] acquire_operation acquire() {
        return acquire_operation(state_);
    }

    void release(std::size_t permits = 1) noexcept {
        release_to_state(state_, permits);
    }

    [[nodiscard]] std::size_t permits() const noexcept {
        return state_ ? state_->permits : 0;
    }

    [[nodiscard]] std::size_t waiters() const noexcept {
        return state_ ? state_->waiters.size() : 0;
    }

    [[nodiscard]] std::size_t max_waiters() const noexcept {
        return state_ ? state_->max_waiters : 0;
    }

private:
    [[nodiscard]] static bool try_complete_immediate(
        const std::shared_ptr<waiter_state>& waiter) {
        if (!waiter || waiter->detached || waiter->has_result) {
            return true;
        }

        auto owner = waiter->owner.lock();
        if (!owner || !owner->alive) {
            (void)waiter->complete(std::unexpected(make_error(
                vio_error_code::invalid_state, "semaphore acquire owner is not alive")));
            waiter->owner.reset();
            return true;
        }

        if (owner->permits != 0) {
            --owner->permits;
            (void)waiter->complete({});
            waiter->owner.reset();
            return true;
        }

        if (owner->waiters.size() >= owner->max_waiters) {
            (void)waiter->complete(std::unexpected(make_error(
                vio_error_code::resource_exhausted, "semaphore wait queue is full")));
            waiter->owner.reset();
            return true;
        }

        return false;
    }

    [[nodiscard]] static bool enqueue_waiter(const std::shared_ptr<waiter_state>& waiter,
                                             std::coroutine_handle<> continuation,
                                             scheduler_ref scheduler) {
        if (try_complete_immediate(waiter)) {
            return false;
        }

        auto owner = waiter->owner.lock();
        if (!owner || !owner->alive) {
            (void)waiter->complete(std::unexpected(make_error(
                vio_error_code::invalid_state, "semaphore acquire owner is not alive")));
            waiter->owner.reset();
            return false;
        }

        waiter->continuation = continuation;
        waiter->scheduler = scheduler;
        waiter->enqueued = true;
        owner->waiters.push_back(waiter);
        return true;
    }

    static void detach_waiter(std::shared_ptr<semaphore_state> owner,
                              const std::shared_ptr<waiter_state>& waiter) noexcept {
        if (!waiter) {
            return;
        }

        if (waiter->enqueued) {
            auto found = std::find_if(owner->waiters.begin(), owner->waiters.end(),
                                      [&waiter](const std::shared_ptr<waiter_state>& queued) {
                                          return queued.get() == waiter.get();
                                      });
            if (found != owner->waiters.end()) {
                owner->waiters.erase(found);
            }
            waiter->enqueued = false;
            waiter->owner.reset();
            return;
        }

        if (waiter->permit_reserved) {
            waiter->permit_reserved = false;
            waiter->owner.reset();
            if (owner->alive) {
                release_to_state(std::move(owner), 1);
            }
            return;
        }

        waiter->owner.reset();
    }

    static void release_to_state(const std::shared_ptr<semaphore_state>& owner,
                                 std::size_t permits) noexcept {
        if (!owner || !owner->alive || permits == 0) {
            return;
        }

        std::size_t remaining = permits;
        while (remaining != 0 && !owner->waiters.empty()) {
            auto waiter = std::move(owner->waiters.front());
            owner->waiters.pop_front();
            if (!prepare_waiter_for_resume(waiter)) {
                continue;
            }

            --remaining;
            auto scheduler = *waiter->scheduler;
            waiter->permit_reserved = true;
            waiter->enqueued = false;
            (void)waiter->complete({});

            schedule_waiter(std::move(waiter), scheduler);
        }

        add_permits_saturating(*owner, remaining);
    }

    static void close_state(std::shared_ptr<semaphore_state> owner) noexcept {
        if (!owner) {
            return;
        }

        owner->alive = false;
        while (!owner->waiters.empty()) {
            auto waiter = std::move(owner->waiters.front());
            owner->waiters.pop_front();
            if (!prepare_waiter_for_resume(waiter)) {
                continue;
            }

            auto scheduler = *waiter->scheduler;
            waiter->owner.reset();
            waiter->enqueued = false;
            (void)waiter->complete(std::unexpected(make_error(
                vio_error_code::invalid_state, "semaphore destroyed with pending acquire")));

            schedule_waiter(std::move(waiter), scheduler);
        }
    }

    [[nodiscard]] static bool prepare_waiter_for_resume(
        const std::shared_ptr<waiter_state>& waiter) noexcept {
        if (!waiter) {
            return false;
        }

        waiter->enqueued = false;
        if (waiter->detached || !waiter->continuation || !waiter->scheduler.has_value()) {
            waiter->owner.reset();
            return false;
        }

        return true;
    }

    static void schedule_waiter(std::shared_ptr<waiter_state> waiter,
                                scheduler_ref scheduler) noexcept {
        auto scheduled = trampoline::schedule_system(
            scheduler, [state = std::move(waiter), scheduler] mutable {
                resume_waiter(std::move(state), scheduler);
            });
        if (!scheduled.has_value()) {
            std::terminate();
        }
    }

    static void resume_waiter(std::shared_ptr<waiter_state> waiter, scheduler_ref scheduler) {
        if (!waiter || waiter->detached || waiter->observing || !waiter->continuation) {
            return;
        }

        auto continuation = std::exchange(waiter->continuation, {});
        waiter->scheduler.reset();
        current_scheduler_scope scope(scheduler);
        continuation.resume();
    }

    static void add_permits_saturating(semaphore_state& owner, std::size_t permits) noexcept {
        const auto max_permits = std::numeric_limits<std::size_t>::max();
        if (permits > max_permits - owner.permits) {
            owner.permits = max_permits;
            return;
        }

        owner.permits += permits;
    }

    std::shared_ptr<semaphore_state> state_;
};

} // namespace voris::io
