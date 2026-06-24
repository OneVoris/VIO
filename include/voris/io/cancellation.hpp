#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace voris::io {

enum class cancellation_reason : std::uint8_t {
    manual = 1,
    deadline = 2,
    scope_shutdown = 3,
    runtime_shutdown = 4,
    backend_abort = 5,
};

[[nodiscard]] std::string_view to_string(cancellation_reason reason) noexcept;

class cancellation_token;

namespace detail {

class cancellation_state;
struct cancellation_callback_slot;

[[nodiscard]] bool cancellation_internal_lock_held_for_testing(
    const cancellation_token& token) noexcept;

} // namespace detail

using cancellation_callback = std::move_only_function<void(cancellation_reason)>;

class cancellation_registration {
public:
    cancellation_registration() noexcept = default;
    ~cancellation_registration();

    cancellation_registration(const cancellation_registration&) = delete;
    cancellation_registration& operator=(const cancellation_registration&) = delete;

    cancellation_registration(cancellation_registration&& other) noexcept;
    cancellation_registration& operator=(cancellation_registration&& other) noexcept;

    void unregister() noexcept;

    [[nodiscard]] bool active() const;

private:
    friend class cancellation_token;

    cancellation_registration(std::shared_ptr<detail::cancellation_state> state,
                              std::shared_ptr<detail::cancellation_callback_slot> slot) noexcept;

    std::shared_ptr<detail::cancellation_state> state_{};
    std::shared_ptr<detail::cancellation_callback_slot> slot_{};
};

class cancellation_token {
public:
    cancellation_token() noexcept = default;

    [[nodiscard]] bool can_be_cancelled() const noexcept;
    [[nodiscard]] bool cancellation_requested() const;
    [[nodiscard]] std::optional<cancellation_reason> reason() const;

    [[nodiscard]] cancellation_registration register_callback(cancellation_callback callback) const;

private:
    friend class cancellation_source;
    friend bool detail::cancellation_internal_lock_held_for_testing(
        const cancellation_token& token) noexcept;

    explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) noexcept;

    std::shared_ptr<detail::cancellation_state> state_{};
};

class cancellation_source {
public:
    cancellation_source();
    ~cancellation_source() = default;

    cancellation_source(const cancellation_source&) noexcept = default;
    cancellation_source& operator=(const cancellation_source&) noexcept = default;
    cancellation_source(cancellation_source&&) noexcept = default;
    cancellation_source& operator=(cancellation_source&&) noexcept = default;

    [[nodiscard]] bool can_request_cancellation() const noexcept;
    [[nodiscard]] cancellation_token token() const noexcept;
    [[nodiscard]] bool cancellation_requested() const;
    [[nodiscard]] std::optional<cancellation_reason> reason() const;

    [[nodiscard]] bool request_cancellation(cancellation_reason reason = cancellation_reason::manual);

private:
    std::shared_ptr<detail::cancellation_state> state_;
};

} // namespace voris::io
