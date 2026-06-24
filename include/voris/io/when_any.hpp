#pragma once

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <voris/io/cancellation.hpp>
#include <voris/io/task.hpp>
#include <voris/io/trampoline.hpp>

namespace voris::io {

template<class... Results>
struct when_any_result {
    std::size_t index{};
    std::variant<Results...> result;
};

namespace detail {

template<class... Results>
class when_any_state : public std::enable_shared_from_this<when_any_state<Results...>> {
public:
    using result_tuple = std::tuple<Results...>;
    using result_variant = std::variant<Results...>;
    using public_result = when_any_result<Results...>;

    template<std::size_t Index>
    void publish(std::tuple_element_t<Index, result_tuple> result) {
        std::optional<scheduler_ref> scheduler;
        {
            std::scoped_lock lock(mutex_);
            if (winner_.has_value()) {
                return;
            }

            winner_index_ = Index;
            winner_.emplace(std::in_place_index<Index>, std::move(result));
            if (!detached_ && parent_ && parent_scheduler_.has_value()) {
                scheduler = parent_scheduler_;
            }
        }

        if (scheduler.has_value()) {
            auto state = this->shared_from_this();
            trampoline::schedule(*scheduler, [state = std::move(state)] {
                state->resume_parent();
            });
        }
    }

    [[nodiscard]] bool has_winner() const noexcept {
        std::scoped_lock lock(mutex_);
        return winner_.has_value();
    }

    [[nodiscard]] bool install_parent(std::coroutine_handle<> continuation,
                                      scheduler_ref scheduler) noexcept {
        std::scoped_lock lock(mutex_);
        if (detached_ || winner_.has_value()) {
            return false;
        }

        parent_ = continuation;
        parent_scheduler_ = scheduler;
        return true;
    }

    void detach_parent() noexcept {
        std::scoped_lock lock(mutex_);
        detached_ = true;
        parent_ = {};
        parent_scheduler_.reset();
    }

    public_result take_winner() {
        std::scoped_lock lock(mutex_);
        return public_result{winner_index_, std::move(*winner_)};
    }

private:
    [[nodiscard]] bool claim_parent(std::coroutine_handle<>& continuation,
                                    scheduler_ref& scheduler) noexcept {
        std::scoped_lock lock(mutex_);
        if (detached_ || !parent_ || !parent_scheduler_.has_value()) {
            return false;
        }

        continuation = parent_;
        scheduler = *parent_scheduler_;
        parent_ = {};
        parent_scheduler_.reset();
        return true;
    }

    void resume_parent() {
        std::coroutine_handle<> continuation{};
        scheduler_ref scheduler{};
        if (!claim_parent(continuation, scheduler)) {
            return;
        }

        current_scheduler_scope scope(scheduler);
        continuation.resume();
    }

    mutable std::mutex mutex_{};
    std::optional<result_variant> winner_{};
    std::size_t winner_index_{0};
    std::coroutine_handle<> parent_{};
    std::optional<scheduler_ref> parent_scheduler_{};
    bool detached_{false};
};

template<class... Results>
class when_any_winner_awaiter {
public:
    using state_type = when_any_state<Results...>;
    using public_result = when_any_result<Results...>;

    explicit when_any_winner_awaiter(std::shared_ptr<state_type> state) noexcept
        : state_(std::move(state)) {}

    when_any_winner_awaiter(const when_any_winner_awaiter&) = delete;
    when_any_winner_awaiter& operator=(const when_any_winner_awaiter&) = delete;

    when_any_winner_awaiter(when_any_winner_awaiter&& other) noexcept
        : state_(std::move(other.state_)) {}

    when_any_winner_awaiter& operator=(when_any_winner_awaiter&& other) noexcept {
        if (this != &other) {
            detach();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~when_any_winner_awaiter() {
        detach();
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return state_ == nullptr || state_->has_winner();
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        auto scheduler = current_scheduler();
        if (!scheduler.has_value()) {
            return false;
        }

        return state_->install_parent(continuation, *scheduler);
    }

    public_result await_resume() {
        return state_->take_winner();
    }

private:
    void detach() noexcept {
        if (state_ != nullptr) {
            state_->detach_parent();
            state_.reset();
        }
    }

    std::shared_ptr<state_type> state_;
};

template<std::size_t Index, class ChildTask, class State>
[[nodiscard]] task<void> observe_when_any_child(ChildTask child, std::shared_ptr<State> state) {
    auto result = co_await std::move(child);
    state->template publish<Index>(std::move(result));
    co_return;
}

template<std::size_t Index = 0, class TaskTuple, class State, class... Observers>
[[nodiscard]] auto make_when_any_observers(TaskTuple& tasks,
                                           std::shared_ptr<State> state,
                                           Observers&&... observers) {
    if constexpr (Index == std::tuple_size_v<TaskTuple>) {
        return std::tuple<std::remove_cvref_t<Observers>...>{
            std::forward<Observers>(observers)...};
    } else {
        auto observer =
            observe_when_any_child<Index>(std::move(std::get<Index>(tasks)), state);
        return make_when_any_observers<Index + 1>(tasks,
                                                  std::move(state),
                                                  std::forward<Observers>(observers)...,
                                                  std::move(observer));
    }
}

template<class TaskTuple, std::size_t... Indices>
[[nodiscard]] auto when_any_impl(cancellation_source& losers_source,
                                 TaskTuple tasks,
                                 std::index_sequence<Indices...>)
    -> task<when_any_result<typename std::tuple_element_t<Indices, TaskTuple>::result_type...>> {
    using state_type =
        when_any_state<typename std::tuple_element_t<Indices, TaskTuple>::result_type...>;
    auto state = std::make_shared<state_type>();
    auto observers = make_when_any_observers(tasks, state);

    auto winner =
        co_await when_any_winner_awaiter<
            typename std::tuple_element_t<Indices, TaskTuple>::result_type...>{state};
    (void)losers_source.request_cancellation(cancellation_reason::manual);

    ((void)(co_await std::move(std::get<Indices>(observers))), ...);
    co_return std::move(winner);
}

} // namespace detail

template<class FirstTask, class... RestTasks>
[[nodiscard]] auto when_any(cancellation_source& losers_source,
                            FirstTask&& first,
                            RestTasks&&... rest) {
    using task_tuple = std::tuple<std::remove_cvref_t<FirstTask>,
                                  std::remove_cvref_t<RestTasks>...>;
    return detail::when_any_impl(losers_source,
                                 task_tuple{std::forward<FirstTask>(first),
                                            std::forward<RestTasks>(rest)...},
                                 std::index_sequence_for<FirstTask, RestTasks...>{});
}

} // namespace voris::io
