#pragma once

#include <coroutine>
#include <exception>
#include <expected>
#include <optional>
#include <type_traits>
#include <utility>

#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

namespace voris::io {

namespace detail {

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
        if (!promise.continuation || !promise.continuation_scheduler.has_value()) {
            return;
        }

        const std::coroutine_handle<> continuation = promise.continuation;
        const scheduler_ref scheduler = *promise.continuation_scheduler;
        trampoline::schedule(scheduler, [continuation, scheduler] {
            current_scheduler_scope scope(scheduler);
            continuation.resume();
        });
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
            : handle_(handle) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : handle_(std::exchange(other.handle_, {})) {}

        ~awaiter() {
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

            handle_.promise().install_continuation(continuation, *scheduler);
            return true;
        }

        result_type await_resume() {
            if (!handle_) {
                return detail::invalid_result<result_type>("empty task");
            }

            auto handle = std::exchange(handle_, {});
            result_type result = handle.promise().take_result();
            handle.destroy();
            return result;
        }

    private:
        handle_type handle_{};
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
    std::optional<scheduler_ref> continuation_scheduler{};
    std::coroutine_handle<> continuation{};
    std::optional<result_type> result{};
    bool consumed{false};

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
        return result.has_value();
    }

    void install_continuation(std::coroutine_handle<> next, scheduler_ref scheduler) {
        if (consumed) {
            set_invalid_state("task awaited more than once");
            return;
        }
        consumed = true;
        continuation = next;
        continuation_scheduler = scheduler;
    }

    result_type take_result() {
        if (!result.has_value()) {
            return detail::invalid_result<result_type>("task has not completed");
        }
        return std::move(*result);
    }

    void set_invalid_state(std::string diagnostic) {
        if (!result.has_value()) {
            result.emplace(std::unexpected(make_error(vio_error_code::invalid_state,
                                                      std::move(diagnostic))));
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
        explicit awaiter(handle_type handle) noexcept
            : handle_(handle) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : handle_(std::exchange(other.handle_, {})) {}

        ~awaiter() {
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool await_ready() const noexcept;
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        result_type await_resume();

    private:
        handle_type handle_{};
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
    std::optional<scheduler_ref> continuation_scheduler{};
    std::coroutine_handle<> continuation{};
    std::optional<result_type> result{};
    bool consumed{false};

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
        return result.has_value();
    }

    void install_continuation(std::coroutine_handle<> next, scheduler_ref scheduler) {
        if (consumed) {
            set_invalid_state("task awaited more than once");
            return;
        }
        consumed = true;
        continuation = next;
        continuation_scheduler = scheduler;
    }

    result_type take_result() {
        if (!result.has_value()) {
            return detail::invalid_result<result_type>("task has not completed");
        }
        return std::move(*result);
    }

    void set_invalid_state(std::string diagnostic) {
        if (!result.has_value()) {
            result.emplace(std::unexpected(make_error(vio_error_code::invalid_state,
                                                      std::move(diagnostic))));
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

inline bool task<void>::awaiter::await_ready() const noexcept {
    return !handle_ || handle_.promise().has_result();
}

inline bool task<void>::awaiter::await_suspend(std::coroutine_handle<> continuation) {
    auto scheduler = current_scheduler();
    if (!scheduler.has_value()) {
        handle_.promise().set_invalid_state("await without a current scheduler");
        return false;
    }

    handle_.promise().install_continuation(continuation, *scheduler);
    return true;
}

inline task<void>::result_type task<void>::awaiter::await_resume() {
    if (!handle_) {
        return detail::invalid_result<result_type>("empty task");
    }

    auto handle = std::exchange(handle_, {});
    result_type result = handle.promise().take_result();
    handle.destroy();
    return result;
}

} // namespace voris::io
