#include <voris/io/detail/mailbox.hpp>

#include "test_assert.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

int main() {
    using namespace voris::io;

    detail::mailbox mailbox(2);
    std::vector<int> order;
    auto first = std::make_unique<int>(1);
    auto second = std::make_unique<int>(2);
    auto third = std::make_unique<int>(3);
    assert(mailbox.capacity() == 2);
    assert(mailbox.size() == 0);
    assert(!mailbox.full());
    assert(mailbox.submit([&order] { order.push_back(1); }).has_value());
    assert(mailbox.submit([&order, second = std::move(second)] { order.push_back(*second); })
               .has_value());
    assert(mailbox.full());
    auto full = mailbox.submit([&order, third = std::move(third)] { order.push_back(*third); });
    assert(!full.has_value());
    assert(full.error().classification == vio_error_code::resource_exhausted);
    assert(mailbox.capacity_waiters() == 1);

    assert(mailbox.run_one());
    assert(order == std::vector<int>{1});
    assert(mailbox.capacity_waiters() == 0);
    assert(mailbox.submit([&order, first = std::move(first)] { order.push_back(*first + 2); })
               .has_value());
    assert(mailbox.run_until_idle() == 2);
    assert(order == std::vector<int>({1, 2, 3}));

    detail::mailbox reentrant(1);
    std::vector<int> reentrant_order;
    assert(reentrant.submit([&] {
                        reentrant_order.push_back(1);
                        assert(reentrant.submit([&] { reentrant_order.push_back(2); }).has_value());
                        assert(reentrant.run_one());
                        reentrant_order.push_back(3);
                    })
               .has_value());
    assert(reentrant.run_one());
    assert(reentrant.run_until_idle() == 0);
    assert(reentrant_order == std::vector<int>({1, 2, 3}));

    detail::mailbox threaded(16);
    constexpr int total = 256;
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<int> executed{0};
    std::vector<int> seen(total, 0);

    std::thread producer([&] {
        for (int index = 0; index < total; ++index) {
            for (;;) {
                auto id = std::make_unique<int>(index);
                auto result = threaded.submit([&seen, &executed, id = std::move(id)] {
                    assert(*id >= 0);
                    assert(*id < total);
                    ++seen[static_cast<std::size_t>(*id)];
                    executed.fetch_add(1);
                });
                if (result.has_value()) {
                    accepted.fetch_add(1);
                    break;
                }
                assert(result.error().classification == vio_error_code::resource_exhausted);
                rejected.fetch_add(1);
                std::this_thread::yield();
            }
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (accepted.load() < total || threaded.size() != 0) {
        if (!threaded.run_one()) {
            std::this_thread::yield();
        }
        assert(std::chrono::steady_clock::now() < deadline);
    }
    producer.join();
    while (threaded.run_one()) {}

    assert(accepted.load() == total);
    assert(executed.load() == total);
    assert(threaded.size() == 0);
    for (int count : seen) {
        assert(count == 1);
    }

    detail::mailbox full_smoke(1);
    int skipped = 0;
    assert(full_smoke.submit([] {}).has_value());
    auto full_smoke_result = full_smoke.submit([&skipped] { ++skipped; });
    assert(!full_smoke_result.has_value());
    assert(full_smoke_result.error().classification == vio_error_code::resource_exhausted);
    assert(full_smoke.capacity_waiters() == 1);
    assert(skipped == 0);

    return 0;
}
