#pragma once

#include <coroutine>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

namespace voris::io {

namespace detail {

enum class task_continuation_install_result {
    suspend,
    ready,
    already_consumed,
};

// Queued final-suspend work can outlive the child frame; this state lets an
// abandoning parent detach the continuation before the frame is destroyed.
struct task_continuation_state {
    mutable std::mutex mutex{};
    std::coroutine_handle<> continuation{};
    std::optional<scheduler_ref> scheduler{};
    bool completed{false};
    bool detached{false};
    bool consumed{false};

    void detach() noexcept {
        std::scoped_lock lock(mutex);
        continuation = {};
        scheduler.reset();
        detached = true;
    }

    [[nodiscard]] bool is_completed() const noexcept {
        std::scoped_lock lock(mutex);
        return completed;
    }

    [[nodiscard]] task_continuation_install_result install(std::coroutine_handle<> next,
                                                          scheduler_ref next_scheduler) noexcept {
        std::scoped_lock lock(mutex);
        if (consumed) {
            return task_continuation_install_result::already_consumed;
        }

        consumed = true;
        if (completed || detached) {
            return task_continuation_install_result::ready;
        }

        continuation = next;
        scheduler = next_scheduler;
        return task_continuation_install_result::suspend;
    }

    [[nodiscard]] std::optional<scheduler_ref> publish_completed() noexcept {
        std::scoped_lock lock(mutex);
        completed = true;
        if (detached || !continuation || !scheduler.has_value()) {
            return std::nullopt;
        }
        return *scheduler;
    }

    [[nodiscard]] bool claim(std::coroutine_handle<>& claimed,
                             scheduler_ref& claimed_scheduler) noexcept {
        std::scoped_lock lock(mutex);
        if (detached || !continuation || !scheduler.has_value()) {
            return false;
        }

        claimed = continuation;
        claimed_scheduler = *scheduler;
        continuation = {};
        scheduler.reset();
        return true;
    }

    void mark_completed() noexcept {
        std::scoped_lock lock(mutex);
        completed = true;
    }
};

inline void resume_task_continuation(std::shared_ptr<task_continuation_state> state) {
    std::coroutine_handle<> continuation{};
    scheduler_ref scheduler{};
    if (!state || !state->claim(continuation, scheduler)) {
        return;
    }

    current_scheduler_scope scope(scheduler);
    continuation.resume();
}

template<class Promise>
class task_initial_awaiter {
public:
    [[nodiscard]] bool await_ready() const noexcept {
        return promise_->creation_scheduler.has_value();
    }

    void await_suspend(std::coroutine_handle<Promise>) noexcept {
        promise_->set_invalid_state("task created without a current scheduler");
    }

    void await_resume() const noexcept {}

    Promise* promise_;
};

template<class Promise>
class task_final_awaiter {
public:
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<Promise> completed) const noexcept {
        auto& promise = completed.promise();
        auto state = promise.continuation_state;
        if (!state) {
            return;
        }

        auto scheduler = state->publish_completed();
        if (!scheduler.has_value()) {
            return;
        }

        auto scheduled = trampoline::schedule_system(*scheduler, [state = std::move(state)] {
            resume_task_continuation(std::move(state));
        });
        if (!scheduled.has_value()) {
            // Reserved/system continuation capacity exhaustion is an internal invariant
            // violation until a richer M8 diagnostic policy exists.
            std::terminate();
        }
    }

    void await_resume() const noexcept {}
};

template<class Result>
[[nodiscard]] Result invalid_result(std::string diagnostic) {
    return std::unexpected(make_error(vio_error_code::invalid_state, std::move(diagnostic)));
}

} // namespace detail

template<class T = void>
class task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
    using result_type = io_result<T>;

    task() noexcept = default;

    explicit task(handle_type handle) noexcept
        : handle_(handle) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~task() {
        destroy();
    }

    [[nodiscard]] bool empty() const noexcept {
        return !handle_;
    }

    [[nodiscard]] bool is_ready() const noexcept;

    result_type take_result() && {
        if (!handle_) {
            return detail::invalid_result<result_type>("empty task");
        }

        auto handle = std::exchange(handle_, {});
        result_type result = handle.promise().take_result();
        handle.destroy();
        return result;
    }

    class awaiter {
    public:
        explicit awaiter(handle_type handle) noexcept
            : handle_(handle),
              continuation_state_(handle ? handle.promise().continuation_state : nullptr) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : handle_(std::exchange(other.handle_, {})),
              continuation_state_(std::move(other.continuation_state_)) {}

        ~awaiter() {
            detach();
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return !handle_ || handle_.promise().has_result();
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            auto scheduler = current_scheduler();
            if (!scheduler.has_value()) {
                handle_.promise().set_invalid_state("await without a current scheduler");
                return false;
            }

            return handle_.promise().install_continuation(continuation, *scheduler);
        }

        result_type await_resume() {
            if (!handle_) {
                return detail::invalid_result<result_type>("empty task");
            }

            auto handle = std::exchange(handle_, {});
            result_type result = handle.promise().take_result();
            handle.destroy();
            detach();
            return result;
        }

    private:
        void detach() noexcept {
            if (continuation_state_) {
                continuation_state_->detach();
                continuation_state_.reset();
            }
        }

        handle_type handle_{};
        std::shared_ptr<detail::task_continuation_state> continuation_state_{};
    };

    awaiter operator co_await() && noexcept {
        return awaiter(std::exchange(handle_, {}));
    }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    handle_type handle_{};
};

template<class T>
struct task<T>::promise_type {
    std::optional<scheduler_ref> creation_scheduler{current_scheduler()};
    std::shared_ptr<detail::task_continuation_state> continuation_state{
        std::make_shared<detail::task_continuation_state>()};
    std::optional<result_type> result{};

    task get_return_object() noexcept {
        return task(handle_type::from_promise(*this));
    }

    auto initial_suspend() noexcept {
        return detail::task_initial_awaiter<promise_type>{this};
    }

    auto final_suspend() noexcept {
        return detail::task_final_awaiter<promise_type>{};
    }

    void return_value(T value) {
        result.emplace(std::move(value));
    }

    void return_value(result_type value) {
        result.emplace(std::move(value));
    }

    void unhandled_exception() {
        result.emplace(std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "unhandled coroutine exception")));
    }

    [[nodiscard]] bool has_result() const noexcept {
        return continuation_state->is_completed();
    }

    [[nodiscard]] bool install_continuation(std::coroutine_handle<> next, scheduler_ref scheduler) {
        const auto installed = continuation_state->install(next, scheduler);
        if (installed == detail::task_continuation_install_result::already_consumed) {
            set_invalid_state("task awaited more than once");
            return false;
        }
        return installed == detail::task_continuation_install_result::suspend;
    }

    result_type take_result() {
        if (!has_result() || !result.has_value()) {
            return detail::invalid_result<result_type>("task has not completed");
        }
        return std::move(*result);
    }

    void set_invalid_state(std::string diagnostic) {
        if (!result.has_value()) {
            result.emplace(std::unexpected(make_error(vio_error_code::invalid_state,
                                                      std::move(diagnostic))));
            continuation_state->mark_completed();
        }
    }
};

template<class T>
inline bool task<T>::is_ready() const noexcept {
    return !handle_ || handle_.promise().has_result();
}

template<>
class task<void> {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
    using result_type = void_result;

    task() noexcept = default;

    explicit task(handle_type handle) noexcept
        : handle_(handle) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~task() {
        destroy();
    }

    [[nodiscard]] bool empty() const noexcept {
        return !handle_;
    }

    [[nodiscard]] bool is_ready() const noexcept;

    result_type take_result() &&;

    class awaiter {
    public:
        explicit awaiter(handle_type handle) noexcept;

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : handle_(std::exchange(other.handle_, {})),
              continuation_state_(std::move(other.continuation_state_)) {}

        ~awaiter() {
            detach();
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool await_ready() const noexcept;
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        result_type await_resume();

    private:
        void detach() noexcept {
            if (continuation_state_) {
                continuation_state_->detach();
                continuation_state_.reset();
            }
        }

        handle_type handle_{};
        std::shared_ptr<detail::task_continuation_state> continuation_state_{};
    };

    awaiter operator co_await() && noexcept {
        return awaiter(std::exchange(handle_, {}));
    }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    handle_type handle_{};
};

struct task<void>::promise_type {
    std::optional<scheduler_ref> creation_scheduler{current_scheduler()};
    std::shared_ptr<detail::task_continuation_state> continuation_state{
        std::make_shared<detail::task_continuation_state>()};
    std::optional<result_type> result{};

    task get_return_object() noexcept {
        return task(handle_type::from_promise(*this));
    }

    auto initial_suspend() noexcept {
        return detail::task_initial_awaiter<promise_type>{this};
    }

    auto final_suspend() noexcept {
        return detail::task_final_awaiter<promise_type>{};
    }

    void return_void() {
        result.emplace();
    }

    void unhandled_exception() {
        result.emplace(std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "unhandled coroutine exception")));
    }

    [[nodiscard]] bool has_result() const noexcept {
        return continuation_state->is_completed();
    }

    [[nodiscard]] bool install_continuation(std::coroutine_handle<> next, scheduler_ref scheduler) {
        const auto installed = continuation_state->install(next, scheduler);
        if (installed == detail::task_continuation_install_result::already_consumed) {
            set_invalid_state("task awaited more than once");
            return false;
        }
        return installed == detail::task_continuation_install_result::suspend;
    }

    result_type take_result() {
        if (!has_result() || !result.has_value()) {
            return detail::invalid_result<result_type>("task has not completed");
        }
        return std::move(*result);
    }

    void set_invalid_state(std::string diagnostic) {
        if (!result.has_value()) {
            result.emplace(std::unexpected(make_error(vio_error_code::invalid_state,
                                                      std::move(diagnostic))));
            continuation_state->mark_completed();
        }
    }
};

inline task<void>::result_type task<void>::take_result() && {
    if (!handle_) {
        return detail::invalid_result<result_type>("empty task");
    }

    auto handle = std::exchange(handle_, {});
    result_type result = handle.promise().take_result();
    handle.destroy();
    return result;
}

inline bool task<void>::is_ready() const noexcept {
    return !handle_ || handle_.promise().has_result();
}

inline task<void>::awaiter::awaiter(task<void>::handle_type handle) noexcept
    : handle_(handle),
      continuation_state_(handle ? handle.promise().continuation_state : nullptr) {}

inline bool task<void>::awaiter::await_ready() const noexcept {
    return !handle_ || handle_.promise().has_result();
}

inline bool task<void>::awaiter::await_suspend(std::coroutine_handle<> continuation) {
    auto scheduler = current_scheduler();
    if (!scheduler.has_value()) {
        handle_.promise().set_invalid_state("await without a current scheduler");
        return false;
    }

    return handle_.promise().install_continuation(continuation, *scheduler);
}

inline task<void>::result_type task<void>::awaiter::await_resume() {
    if (!handle_) {
        return detail::invalid_result<result_type>("empty task");
    }

    auto handle = std::exchange(handle_, {});
    result_type result = handle.promise().take_result();
    handle.destroy();
    detach();
    return result;
}

} // namespace voris::io
