#include <voris/io/deadline.hpp>
#include <voris/io/virtual_clock.hpp>

#if __has_include(<voris/io/cancellation.hpp>)
#include <voris/io/cancellation.hpp>
#define VIO_DEADLINE_TEST_HAS_CANCELLATION 1
#else
#define VIO_DEADLINE_TEST_HAS_CANCELLATION 0
#endif

#include "test_assert.hpp"
#include <chrono>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    const auto none = deadline::none();
    assert(!none.has_value());
    assert(!none.expired(std::chrono::steady_clock::time_point{10ms}));
    assert(!none.remaining(std::chrono::steady_clock::time_point{10ms}).has_value());

    const deadline steady_deadline{std::chrono::steady_clock::time_point{20ms}};
    assert(steady_deadline.has_value());
    assert(!steady_deadline.expired(std::chrono::steady_clock::time_point{19ms}));
    assert(steady_deadline.expired(std::chrono::steady_clock::time_point{20ms}));
    assert(steady_deadline.expired(std::chrono::steady_clock::time_point{21ms}));

    const auto future_remaining =
        steady_deadline.remaining(std::chrono::steady_clock::time_point{12ms});
    assert(future_remaining.has_value());
    assert(*future_remaining == 8ms);

    const auto expired_remaining =
        steady_deadline.remaining(std::chrono::steady_clock::time_point{25ms});
    assert(expired_remaining.has_value());
    assert(*expired_remaining == deadline::duration::zero());

    virtual_monotonic_clock clock{virtual_monotonic_clock::time_point{5ms}};
    const deadline virtual_deadline{virtual_monotonic_clock::time_point{17ms}};
    assert(!virtual_deadline.expired(clock.now()));
    const auto virtual_remaining = virtual_deadline.remaining(clock.now());
    assert(virtual_remaining.has_value());
    assert(*virtual_remaining == 12ms);

    const auto advanced = clock.advance_to(virtual_monotonic_clock::time_point{17ms});
    assert(advanced.has_value());
    assert(virtual_deadline.expired(clock.now()));

    const auto deadline_error = deadline::cancellation_error();
    assert(deadline_error.classification == vio_error_code::deadline_exceeded);

#if VIO_DEADLINE_TEST_HAS_CANCELLATION
    static_assert(deadline::cancellation_reason() == cancellation_reason::deadline);

    cancellation_source source;
    assert(!request_cancellation_if_expired(
        source, steady_deadline, std::chrono::steady_clock::time_point{19ms}));
    assert(!source.cancellation_requested());

    assert(request_cancellation_if_expired(
        source, steady_deadline, std::chrono::steady_clock::time_point{20ms}));
    assert(source.cancellation_requested());
    assert(source.reason() == cancellation_reason::deadline);

    assert(!request_cancellation_if_expired(
        source, steady_deadline, std::chrono::steady_clock::time_point{21ms}));
    assert(source.reason() == cancellation_reason::deadline);
#endif

    return 0;
}
