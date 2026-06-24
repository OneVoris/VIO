#include <voris/io/detail/bounded_queue.hpp>

#include "test_assert.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

int main() {
    using namespace voris::io;

    detail::bounded_queue<int> queue(2);
    assert(queue.capacity() == 2);
    assert(queue.empty());
    assert(!queue.full());
    assert(queue.try_push(1).has_value());
    assert(queue.try_push(2).has_value());
    assert(queue.size() == 2);
    assert(queue.full());
    auto full = queue.try_push(3);
    assert(!full.has_value());
    assert(full.error().classification == vio_error_code::resource_exhausted);
    assert(queue.capacity_waiters() == 1);
    assert(!queue.try_push(4).has_value());
    assert(!queue.try_push(5).has_value());
    assert(queue.capacity_waiters() == 2);

    auto first = queue.pop();
    assert(first.has_value());
    assert(*first == 1);
    assert(queue.capacity_waiters() == 0);

    assert(queue.try_push(3).has_value());
    assert(*queue.pop() == 2);
    assert(*queue.pop() == 3);
    assert(!queue.pop().has_value());

    detail::bounded_queue<int> zero_capacity(0);
    auto zero_full = zero_capacity.try_push(1);
    assert(!zero_full.has_value());
    assert(zero_full.error().classification == vio_error_code::resource_exhausted);
    assert(zero_capacity.capacity_waiters() == 1);
    assert(zero_capacity.empty());
    assert(zero_capacity.full());

    detail::bounded_queue<std::unique_ptr<int>> move_only(1);
    assert(move_only.try_push(std::make_unique<int>(42)).has_value());
    auto payload = move_only.pop();
    assert(payload.has_value());
    assert(**payload == 42);
    assert(move_only.empty());

    detail::bounded_queue<int> concurrent(32);
    constexpr int producer_count = 4;
    constexpr int per_producer = 128;
    constexpr int total = producer_count * per_producer;
    std::atomic<int> accepted{0};
    std::atomic<int> consumed{0};
    std::vector<int> seen(total, 0);

    std::thread consumer([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (consumed.load() < total) {
            if (auto value = concurrent.pop()) {
                assert(*value >= 0);
                assert(*value < total);
                ++seen[static_cast<std::size_t>(*value)];
                consumed.fetch_add(1);
                continue;
            }
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (int offset = 0; offset < per_producer; ++offset) {
                const int value = producer * per_producer + offset;
                for (;;) {
                    auto result = concurrent.try_push(value);
                    if (result.has_value()) {
                        accepted.fetch_add(1);
                        break;
                    }
                    assert(result.error().classification == vio_error_code::resource_exhausted);
                    assert(concurrent.capacity_waiters() <= concurrent.capacity());
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    assert(accepted.load() == total);
    assert(consumed.load() == total);
    assert(concurrent.empty());
    assert(concurrent.capacity_waiters() == 0);
    for (int count : seen) {
        assert(count == 1);
    }

    return 0;
}
