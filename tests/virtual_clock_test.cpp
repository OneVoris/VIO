#include <voris/io/virtual_clock.hpp>

#include "test_assert.hpp"
#include <chrono>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    virtual_monotonic_clock clock;
    assert(clock.now() == virtual_monotonic_clock::time_point{});

    const auto advanced = clock.advance_by(10ms);
    assert(advanced.has_value());
    assert(*advanced == clock.now());
    assert(clock.now().time_since_epoch() == 10ms);

    const auto later = virtual_monotonic_clock::time_point{25ms};
    const auto advanced_to_later = clock.advance_to(later);
    assert(advanced_to_later.has_value());
    assert(*advanced_to_later == later);
    assert(clock.now() == later);

    const auto zero_advance = clock.advance_by(0ns);
    assert(zero_advance.has_value());
    assert(*zero_advance == later);
    const auto same_advance = clock.advance_to(later);
    assert(same_advance.has_value());
    assert(*same_advance == later);

    const auto rejected_advance_to = clock.advance_to(virtual_monotonic_clock::time_point{20ms});
    assert(!rejected_advance_to.has_value());
    assert(rejected_advance_to.error().classification == vio_error_code::invalid_state);
    assert(clock.now() == later);

    const auto rejected_advance_by = clock.advance_by(-1ns);
    assert(!rejected_advance_by.has_value());
    assert(rejected_advance_by.error().classification == vio_error_code::invalid_state);
    assert(clock.now() == later);

    return 0;
}
