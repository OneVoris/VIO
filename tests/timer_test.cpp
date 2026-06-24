#include <voris/io/timer.hpp>

#include "test_assert.hpp"
#include <chrono>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    timer_heap heap;
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto cancelled = heap.add(virtual_monotonic_clock::time_point{10ms});
    assert(heap.cancel(cancelled));
    assert(!heap.cancel(timer_handle{}));
    assert(heap.size() == 3);

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{10ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == first.id());
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{20ms});

    ready = heap.pop_ready(virtual_monotonic_clock::time_point{20ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == second.id());
    assert(!heap.next_deadline().has_value());

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scope(ref);
    virtual_monotonic_clock clock;
    auto slept = sleep_for(clock, 5ms);
    auto result = std::move(slept).take_result();
    assert(result.has_value());
    assert(clock.now() == virtual_monotonic_clock::time_point{5ms});

    auto slept_until = sleep_until(clock, virtual_monotonic_clock::time_point{12ms});
    auto until_result = std::move(slept_until).take_result();
    assert(until_result.has_value());
    assert(clock.now() == virtual_monotonic_clock::time_point{12ms});

    return 0;
}
