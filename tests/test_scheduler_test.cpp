#include <voris/io/test_scheduler.hpp>

#include <cassert>
#include <memory>
#include <vector>

int main() {
    using namespace voris::io;

    test_scheduler scheduler;
    std::vector<int> order;

    scheduler.enqueue([&order] { order.push_back(1); });
    scheduler.enqueue([&order] { order.push_back(2); });

    assert(order.empty());
    assert(scheduler.ready_count() == 2);
    assert(scheduler.run_one());
    assert(order == std::vector<int>{1});
    assert(scheduler.ready_count() == 1);

    scheduler.enqueue([&scheduler, &order] {
        order.push_back(3);
        scheduler.enqueue([&order] { order.push_back(4); });
    });

    assert(scheduler.run_ready() == 2);
    assert(order == std::vector<int>({1, 2, 3}));
    assert(scheduler.ready_count() == 1);

    assert(scheduler.run_until_idle() == 1);
    assert(order == std::vector<int>({1, 2, 3, 4}));
    assert(!scheduler.run_one());

    auto marker = std::make_unique<int>(5);
    scheduler.enqueue([moved = std::move(marker), &order] { order.push_back(*moved); });
    assert(marker == nullptr);
    assert(scheduler.run_until_idle() == 1);
    assert(order == std::vector<int>({1, 2, 3, 4, 5}));

    return 0;
}
