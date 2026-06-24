#pragma once

#include <chrono>

#include <voris/io/error.hpp>

namespace voris::io {

class virtual_monotonic_clock {
public:
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<virtual_monotonic_clock, duration>;

    static constexpr bool is_steady = true;

    constexpr virtual_monotonic_clock() noexcept = default;

    explicit constexpr virtual_monotonic_clock(time_point initial) noexcept
        : current_(initial) {}

    [[nodiscard]] constexpr time_point now() const noexcept {
        return current_;
    }

    [[nodiscard]] io_result<time_point> advance_by(duration delta) {
        if (delta < duration::zero()) {
            return std::unexpected(make_error(vio_error_code::invalid_state,
                                              "virtual clock cannot move backwards"));
        }
        return advance_to(current_ + delta);
    }

    [[nodiscard]] io_result<time_point> advance_to(time_point target) {
        if (target < current_) {
            return std::unexpected(make_error(vio_error_code::invalid_state,
                                              "virtual clock cannot move backwards"));
        }
        current_ = target;
        return current_;
    }

private:
    time_point current_{};
};

} // namespace voris::io
