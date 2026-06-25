#pragma once

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

namespace voris::io {

class async_mutex {
    struct waiter_state;

    struct mutex_state {
        explicit mutex_state(std::size_t waiter_limit) noexcept
            : max_waiters(waiter_limit) {}

        bool locked{false};
        std::size_t max_waiters;
        std::deque<std::shared_ptr<waiter_state>> waiters;
        std::weak_ptr<waiter_state> handoff;
        bool alive{true};
    };

    struct waiter_state {
        explicit waiter_state(const std::shared_ptr<mutex_state>& owner_state) noexcept
            : owner(owner_state) {}

        bool complete(void_result value) {
            if (has_result) {
                return false;
            }

            result = std::move(value);
            has_result = true;
            return true;
        }

        std::weak_ptr<mutex_state> owner;
        std::coroutine_handle<> continuation{};
        std::optional<scheduler_ref> scheduler{};
        void_result result{};
        bool has_result{false};
        bool enqueued{false};
        bool detached{false};
        bool observing{false};
        bool lock_reserved{false};
    };

public:
    class lock_operation {
    public:
        explicit lock_operation(const std::shared_ptr<mutex_state>& owner)
            : state_(std::make_shared<waiter_state>(owner)) {}

        lock_operation(const lock_operation&) = delete;
        lock_operation& operator=(const lock_operation&) = delete;

        lock_operation(lock_operation&& other) noexcept
            : state_(std::move(other.state_)) {}

        lock_operation& operator=(lock_operation&& other) noexcept {
            if (this != &other) {
                detach();
                state_ = std::move(other.state_);
            }
            return *this;
        }

        ~lock_operation() {
            detach();
        }

        [[nodiscard]] bool await_ready() {
            return async_mutex::try_complete_immediate(state_);
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            auto scheduler = current_scheduler();
            if (!scheduler.has_value()) {
                complete_invalid_state("mutex lock without a current scheduler");
                return false;
            }

            return async_mutex::enqueue_waiter(state_, continuation, *scheduler);
        }

        [[nodiscard]] void_result await_resume() {
            if (!state_) {
                return std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "empty mutex lock operation"));
            }

            state_->observing = true;
            state_->continuation = {};
            state_->scheduler.reset();
            if (state_->lock_reserved) {
                if (auto owner = state_->owner.lock()) {
                    async_mutex::clear_handoff(*owner, state_);
                }
                state_->lock_reserved = false;
            }
            state_->owner.reset();

            void_result result = state_->has_result
                                     ? std::move(state_->result)
                                     : std::unexpected(make_error(
                                           vio_error_code::invalid_state,
                                           "mutex lock resumed without completion"));
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
                async_mutex::detach_waiter(std::move(owner), state_);
            } else {
                state_->owner.reset();
            }
            state_.reset();
        }

        std::shared_ptr<waiter_state> state_;
    };

    async_mutex()
        : async_mutex(default_max_waiters()) {}

    // max_waiters == 0 creates a non-queuing mutex: lock() can only complete
    // immediately when unlocked, otherwise it reports resource_exhausted.
    explicit async_mutex(std::size_t max_waiters)
        : state_(std::make_shared<mutex_state>(max_waiters)) {}

    async_mutex(const async_mutex&) = delete;
    async_mutex& operator=(const async_mutex&) = delete;

    ~async_mutex() {
        close_state(std::move(state_));
    }

    [[nodiscard]] static constexpr std::size_t default_max_waiters() noexcept {
        return 1;
    }

    [[nodiscard]] void_result try_lock() {
        if (!state_ || !state_->alive) {
            return std::unexpected(make_error(vio_error_code::invalid_state,
                                              "mutex is not alive"));
        }

        if (state_->locked) {
            return std::unexpected(make_error(vio_error_code::operation_in_progress,
                                              "mutex is already locked"));
        }

        state_->locked = true;
        return {};
    }

    [[nodiscard]] lock_operation lock() {
        return lock_operation(state_);
    }

    void unlock() noexcept {
        if (!state_ || !state_->alive || !state_->locked) {
            return;
        }

        transfer_or_unlock(state_);
    }

    [[nodiscard]] bool locked() const noexcept {
        return state_ ? state_->locked : false;
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
                vio_error_code::invalid_state, "mutex lock owner is not alive")));
            waiter->owner.reset();
            return true;
        }

        if (!owner->locked) {
            owner->locked = true;
            (void)waiter->complete({});
            waiter->owner.reset();
            return true;
        }

        if (owner->waiters.size() >= owner->max_waiters) {
            (void)waiter->complete(std::unexpected(make_error(
                vio_error_code::resource_exhausted, "mutex wait queue is full")));
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
                vio_error_code::invalid_state, "mutex lock owner is not alive")));
            waiter->owner.reset();
            return false;
        }

        waiter->continuation = continuation;
        waiter->scheduler = scheduler;
        waiter->enqueued = true;
        owner->waiters.push_back(waiter);
        return true;
    }

    static void detach_waiter(std::shared_ptr<mutex_state> owner,
                              const std::shared_ptr<waiter_state>& waiter) noexcept {
        if (!waiter) {
            return;
        }

        if (waiter->enqueued) {
            erase_waiter(owner->waiters, waiter);
            waiter->enqueued = false;
            waiter->owner.reset();
            return;
        }

        if (waiter->lock_reserved) {
            waiter->lock_reserved = false;
            clear_handoff(*owner, waiter);
            waiter->owner.reset();
            if (owner->alive) {
                transfer_or_unlock(std::move(owner));
            }
            return;
        }

        waiter->owner.reset();
    }

    static void transfer_or_unlock(const std::shared_ptr<mutex_state>& owner) noexcept {
        if (!owner || !owner->alive) {
            return;
        }

        while (!owner->waiters.empty()) {
            auto waiter = std::move(owner->waiters.front());
            owner->waiters.pop_front();
            if (!prepare_waiter_for_resume(waiter)) {
                continue;
            }

            auto scheduler = *waiter->scheduler;
            waiter->lock_reserved = true;
            owner->locked = true;
            owner->handoff = waiter;

            schedule_waiter(std::move(waiter), scheduler);
            return;
        }

        owner->handoff.reset();
        owner->locked = false;
    }

    static void close_state(std::shared_ptr<mutex_state> owner) noexcept {
        if (!owner) {
            return;
        }

        owner->alive = false;
        if (auto handoff = owner->handoff.lock();
            handoff && handoff->lock_reserved && !handoff->detached &&
            !handoff->observing && !handoff->has_result) {
            handoff->lock_reserved = false;
            handoff->owner.reset();
            (void)handoff->complete(std::unexpected(make_error(
                vio_error_code::invalid_state, "mutex destroyed with pending lock")));
        }
        owner->handoff.reset();

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
                vio_error_code::invalid_state, "mutex destroyed with pending lock")));

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

        complete_reserved_waiter(waiter);
        auto continuation = std::exchange(waiter->continuation, {});
        waiter->scheduler.reset();
        current_scheduler_scope scope(scheduler);
        continuation.resume();
    }

    static void complete_reserved_waiter(const std::shared_ptr<waiter_state>& waiter) {
        if (!waiter || !waiter->lock_reserved || waiter->has_result) {
            return;
        }

        auto owner = waiter->owner.lock();
        if (!owner || !owner->alive) {
            waiter->lock_reserved = false;
            waiter->owner.reset();
            (void)waiter->complete(std::unexpected(make_error(
                vio_error_code::invalid_state, "mutex lock owner is not alive")));
            return;
        }

        (void)waiter->complete({});
    }

    static void erase_waiter(std::deque<std::shared_ptr<waiter_state>>& waiters,
                             const std::shared_ptr<waiter_state>& waiter) noexcept {
        auto found = std::find_if(waiters.begin(), waiters.end(),
                                  [&waiter](const std::shared_ptr<waiter_state>& queued) {
                                      return queued.get() == waiter.get();
                                  });
        if (found != waiters.end()) {
            waiters.erase(found);
        }
    }

    static void clear_handoff(mutex_state& owner,
                              const std::shared_ptr<waiter_state>& waiter) noexcept {
        auto handoff = owner.handoff.lock();
        if (handoff && handoff.get() == waiter.get()) {
            owner.handoff.reset();
        }
    }

    std::shared_ptr<mutex_state> state_;
};

} // namespace voris::io
