#include <voris/io/timer.hpp>

#include <chrono>
#include <iostream>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    timer_heap heap;
    constexpr int count = 1000;
    for (int i = 0; i != count; ++i) {
        (void)heap.add(virtual_monotonic_clock::time_point{i * 1ms});
    }
    const auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{count * 1ms});
    std::cout << "timer_heap_ready=" << ready.size() << '\n';
    return ready.size() == count ? 0 : 1;
}
