#pragma once

#include <chrono>
#include <concepts>
#include <optional>
#include <source_location>

#if __has_include(<voris/io/cancellation.hpp>)
#include <voris/io/cancellation.hpp>
#endif

#include <voris/io/error.hpp>

namespace voris::io {

struct deadline {
    using clock = std::chrono::steady_clock;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<clock, duration>;

    constexpr deadline() noexcept = default;

    template<class Clock, class Duration>
        requires(Clock::is_steady)
    explicit constexpr deadline(std::chrono::time_point<Clock, Duration> when) noexcept
        : when_(normalize(when)) {}

    [[nodiscard]] static constexpr deadline none() noexcept {
        return deadline{};
    }

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return when_.has_value();
    }

    [[nodiscard]] constexpr std::optional<time_point> time() const noexcept {
        return when_;
    }

    template<class Clock, class Duration>
        requires(Clock::is_steady)
    [[nodiscard]] constexpr bool expired(std::chrono::time_point<Clock, Duration> now) const noexcept {
        return when_.has_value() && normalize(now) >= *when_;
    }

    template<class Clock, class Duration>
        requires(Clock::is_steady)
    [[nodiscard]] constexpr std::optional<duration> remaining(
        std::chrono::time_point<Clock, Duration> now) const noexcept {
        if (!when_.has_value()) {
            return std::nullopt;
        }

        const auto normalized_now = normalize(now);
        if (normalized_now >= *when_) {
            return duration::zero();
        }

        return *when_ - normalized_now;
    }

    [[nodiscard]] static vio_error cancellation_error(
        std::source_location location = std::source_location::current());

#if __has_include(<voris/io/cancellation.hpp>)
    [[nodiscard]] static constexpr voris::io::cancellation_reason cancellation_reason() noexcept {
        return voris::io::cancellation_reason::deadline;
    }
#endif

private:
    template<class Clock, class Duration>
        requires(Clock::is_steady)
    [[nodiscard]] static constexpr time_point normalize(
        std::chrono::time_point<Clock, Duration> point) noexcept {
        return time_point{std::chrono::duration_cast<duration>(point.time_since_epoch())};
    }

    std::optional<time_point> when_{};
};

#if __has_include(<voris/io/cancellation.hpp>)
template<class Clock, class Duration>
    requires(Clock::is_steady)
[[nodiscard]] bool request_cancellation_if_expired(
    cancellation_source& source,
    const deadline& limit,
    std::chrono::time_point<Clock, Duration> now) {
    if (!limit.expired(now)) {
        return false;
    }

    return source.request_cancellation(deadline::cancellation_reason());
}
#endif

} // namespace voris::io
