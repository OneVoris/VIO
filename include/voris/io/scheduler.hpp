#pragma once

#include <cstddef>
#include <concepts>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <voris/io/error.hpp>

namespace voris::io {

using continuation = std::move_only_function<void()>;

namespace detail {

template<class Scheduler>
concept submits_continuation = requires(Scheduler& scheduler, continuation next) {
    { scheduler.submit(std::move(next)) } -> std::same_as<void_result>;
};

template<class Scheduler>
concept submits_system_continuation = requires(Scheduler& scheduler, continuation next) {
    { scheduler.submit_system(std::move(next)) } -> std::same_as<void_result>;
};

template<class Scheduler>
concept result_enqueues_continuation = requires(Scheduler& scheduler, continuation next) {
    { scheduler.enqueue(std::move(next)) } -> std::same_as<void_result>;
};

template<class Scheduler>
concept enqueues_continuation = requires(Scheduler& scheduler, continuation next) {
    scheduler.enqueue(std::move(next));
};

template<class Scheduler>
[[nodiscard]] void_result schedule_user_continuation(Scheduler& scheduler, continuation next) {
    if constexpr (submits_continuation<Scheduler>) {
        return scheduler.submit(std::move(next));
    } else if constexpr (result_enqueues_continuation<Scheduler>) {
        return scheduler.enqueue(std::move(next));
    } else {
        scheduler.enqueue(std::move(next));
        return {};
    }
}

template<class Scheduler>
[[nodiscard]] void_result schedule_system_continuation(Scheduler& scheduler, continuation next) {
    if constexpr (submits_system_continuation<Scheduler>) {
        return scheduler.submit_system(std::move(next));
    } else {
        return schedule_user_continuation(scheduler, std::move(next));
    }
}

} // namespace detail

class scheduler_ref {
public:
    scheduler_ref() noexcept = default;
    scheduler_ref(const scheduler_ref&) noexcept = default;
    scheduler_ref& operator=(const scheduler_ref&) noexcept = default;

    template<class Scheduler>
        requires(!std::same_as<std::remove_cvref_t<Scheduler>, scheduler_ref> &&
                 (detail::submits_continuation<Scheduler> ||
                  detail::result_enqueues_continuation<Scheduler> ||
                  detail::enqueues_continuation<Scheduler>))
    explicit scheduler_ref(Scheduler& scheduler) noexcept
        : object_(&scheduler),
          schedule_([](void* object, continuation next) -> void_result {
              return detail::schedule_user_continuation(*static_cast<Scheduler*>(object),
                                                        std::move(next));
          }),
          schedule_system_([](void* object, continuation next) -> void_result {
              return detail::schedule_system_continuation(*static_cast<Scheduler*>(object),
                                                          std::move(next));
          }) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return object_ != nullptr && schedule_ != nullptr && schedule_system_ != nullptr;
    }

    [[nodiscard]] bool operator==(const scheduler_ref& other) const noexcept {
        return object_ == other.object_ && schedule_ == other.schedule_ &&
               schedule_system_ == other.schedule_system_;
    }

    [[nodiscard]] void* identity() const noexcept {
        return object_;
    }

    [[nodiscard]] void_result schedule(continuation next) const {
        if (!*this || !next) {
            return std::unexpected(make_error(vio_error_code::invalid_state));
        }
        return schedule_(object_, std::move(next));
    }

    [[nodiscard]] void_result schedule_system(continuation next) const {
        if (!*this || !next) {
            return std::unexpected(make_error(vio_error_code::invalid_state));
        }
        return schedule_system_(object_, std::move(next));
    }

private:
    using schedule_fn = void_result (*)(void*, continuation);

    void* object_{nullptr};
    schedule_fn schedule_{nullptr};
    schedule_fn schedule_system_{nullptr};
};

[[nodiscard]] std::optional<scheduler_ref> current_scheduler() noexcept;

[[nodiscard]] io_result<scheduler_ref> require_current_scheduler();

void set_current_scheduler_for_testing(std::optional<scheduler_ref> scheduler) noexcept;

class current_scheduler_scope {
public:
    explicit current_scheduler_scope(scheduler_ref scheduler) noexcept;

    current_scheduler_scope(const current_scheduler_scope&) = delete;
    current_scheduler_scope& operator=(const current_scheduler_scope&) = delete;

    current_scheduler_scope(current_scheduler_scope&&) = delete;
    current_scheduler_scope& operator=(current_scheduler_scope&&) = delete;

    ~current_scheduler_scope();

private:
    std::optional<scheduler_ref> previous_;
};

class default_scheduler {
public:
    default_scheduler() = default;

    default_scheduler(const default_scheduler&) = delete;
    default_scheduler& operator=(const default_scheduler&) = delete;

    void enqueue(continuation next);

    [[nodiscard]] bool run_one();
    [[nodiscard]] std::size_t run_until_idle();
    [[nodiscard]] std::size_t ready_count() const noexcept;

private:
    std::deque<continuation> ready_;
};

} // namespace voris::io
