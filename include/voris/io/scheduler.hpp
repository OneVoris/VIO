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

class scheduler_ref {
public:
    scheduler_ref() noexcept = default;
    scheduler_ref(const scheduler_ref&) noexcept = default;
    scheduler_ref& operator=(const scheduler_ref&) noexcept = default;

    template<class Scheduler>
        requires(!std::same_as<std::remove_cvref_t<Scheduler>, scheduler_ref> &&
                 requires(Scheduler& scheduler, continuation next) {
                     scheduler.enqueue(std::move(next));
                 })
    explicit scheduler_ref(Scheduler& scheduler) noexcept
        : object_(&scheduler),
          enqueue_([](void* object, continuation next) {
              static_cast<Scheduler*>(object)->enqueue(std::move(next));
          }) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return object_ != nullptr && enqueue_ != nullptr;
    }

    [[nodiscard]] bool operator==(const scheduler_ref& other) const noexcept {
        return object_ == other.object_ && enqueue_ == other.enqueue_;
    }

    [[nodiscard]] void* identity() const noexcept {
        return object_;
    }

    [[nodiscard]] void_result schedule(continuation next) const {
        if (!*this || !next) {
            return std::unexpected(make_error(vio_error_code::invalid_state));
        }
        enqueue_(object_, std::move(next));
        return {};
    }

private:
    using enqueue_fn = void (*)(void*, continuation);

    void* object_{nullptr};
    enqueue_fn enqueue_{nullptr};
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
