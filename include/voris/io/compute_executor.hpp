#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class compute_executor {
    struct state {
        explicit state(std::size_t queue_limit)
            : capacity(queue_limit) {}

        std::size_t capacity;
        mutable std::mutex mutex;
        std::deque<continuation> queue;
        std::size_t capacity_waiters{0};
        std::size_t reserved_slots{0};
        bool shutting_down{false};
    };

public:
    // Owns one reserved queue slot only; it does not execute queued work.
    class capacity_reservation {
    public:
        capacity_reservation() noexcept = default;

        capacity_reservation(const capacity_reservation&) = delete;
        capacity_reservation& operator=(const capacity_reservation&) = delete;

        capacity_reservation(capacity_reservation&& other) noexcept
            : state_(std::move(other.state_)),
              active_(std::exchange(other.active_, false)) {}

        capacity_reservation& operator=(capacity_reservation&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
                active_ = std::exchange(other.active_, false);
            }
            return *this;
        }

        ~capacity_reservation() {
            release();
        }

        void release() noexcept {
            if (!active_) {
                state_.reset();
                return;
            }

            auto state = std::move(state_);
            active_ = false;
            if (!state) {
                return;
            }

            std::lock_guard lock(state->mutex);
            if (!state->shutting_down && state->reserved_slots != 0) {
                --state->reserved_slots;
                state->capacity_waiters = 0;
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return active_ && state_ != nullptr;
        }

    private:
        friend class compute_executor;

        explicit capacity_reservation(std::shared_ptr<state> state) noexcept
            : state_(std::move(state)),
              active_(true) {}

        [[nodiscard]] bool owns(const std::shared_ptr<state>& state) const noexcept {
            return active_ && state_.get() == state.get();
        }

        void disarm() noexcept {
            active_ = false;
            state_.reset();
        }

        std::shared_ptr<state> state_{};
        bool active_{false};
    };

    explicit compute_executor(std::size_t queue_limit)
        : state_(std::make_shared<state>(queue_limit)) {}

    compute_executor(const compute_executor&) = delete;
    compute_executor& operator=(const compute_executor&) = delete;

    compute_executor(compute_executor&&) = delete;
    compute_executor& operator=(compute_executor&&) = delete;

    [[nodiscard]] void_result submit(continuation work) {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        if (state->shutting_down) {
            return std::unexpected(make_error(vio_error_code::closed,
                                              "compute executor is shutting down"));
        }
        if (!has_capacity_locked(*state)) {
            record_capacity_waiter_locked(*state);
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "compute executor queue is full"));
        }
        state->queue.push_back(std::move(work));
        state->capacity_waiters = 0;
        return {};
    }

    [[nodiscard]] io_result<capacity_reservation> try_reserve_capacity() {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        if (state->shutting_down) {
            return std::unexpected(make_error(vio_error_code::closed,
                                              "compute executor is shutting down"));
        }
        if (!has_capacity_locked(*state)) {
            record_capacity_waiter_locked(*state);
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "compute executor capacity is unavailable"));
        }

        ++state->reserved_slots;
        state->capacity_waiters = 0;
        return capacity_reservation(state);
    }

    // Consumes the reservation by value, including closed or invalid-state failures.
    [[nodiscard]] void_result submit_reserved(capacity_reservation reservation,
                                              continuation work) {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        if (state->shutting_down) {
            if (reservation.owns(state)) {
                reservation.disarm();
            }
            return std::unexpected(make_error(vio_error_code::closed,
                                              "compute executor is shutting down"));
        }
        if (!reservation.owns(state) || state->reserved_slots == 0 ||
            state->queue.size() >= state->capacity) {
            return std::unexpected(make_error(vio_error_code::invalid_state,
                                              "invalid compute executor capacity reservation"));
        }

        --state->reserved_slots;
        reservation.disarm();
        state->queue.push_back(std::move(work));
        state->capacity_waiters = 0;
        return {};
    }

    [[nodiscard]] std::size_t run_until_idle() {
        std::size_t ran = 0;
        for (;;) {
            continuation work;
            {
                auto state = state_;
                std::lock_guard lock(state->mutex);
                if (state->queue.empty()) {
                    break;
                }
                work = std::move(state->queue.front());
                state->queue.pop_front();
                state->capacity_waiters = 0;
            }
            if (work) {
                current_scheduler_scope scope(scheduler_ref(*this));
                work();
            }
            ++ran;
        }
        return ran;
    }

    void request_shutdown() {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        state->shutting_down = true;
        state->capacity_waiters = 0;
        state->reserved_slots = 0;
    }

    void shutdown() {
        request_shutdown();
    }

    [[nodiscard]] bool shutting_down() const {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        return state->shutting_down;
    }

    [[nodiscard]] std::size_t queued() const {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        return state->queue.size();
    }

    [[nodiscard]] std::size_t capacity_waiters() const {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        return state->capacity_waiters;
    }

private:
    [[nodiscard]] static bool has_capacity_locked(const state& state) noexcept {
        if (state.capacity == 0 || state.queue.size() >= state.capacity) {
            return false;
        }
        return state.reserved_slots < state.capacity - state.queue.size();
    }

    static void record_capacity_waiter_locked(state& state) noexcept {
        const std::size_t waiter_limit = state.capacity == 0 ? 1 : state.capacity;
        if (state.capacity_waiters < waiter_limit) {
            ++state.capacity_waiters;
        }
    }

    std::shared_ptr<state> state_;
};

} // namespace voris::io
