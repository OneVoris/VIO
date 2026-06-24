#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

#include <cassert>
#include <memory>
#include <vector>

int main() {
    using namespace voris::io;

    set_current_scheduler_for_testing(std::nullopt);
    assert(!current_scheduler().has_value());
    const auto missing = require_current_scheduler();
    assert(!missing.has_value());
    assert(missing.error().classification == vio_error_code::invalid_state);

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    assert(ref);

    {
        current_scheduler_scope scope(ref);
        const auto current = require_current_scheduler();
        assert(current.has_value());
        assert(current->identity() == ref.identity());

        default_scheduler nested;
        scheduler_ref nested_ref(nested);
        {
            current_scheduler_scope nested_scope(nested_ref);
            const auto nested_current = require_current_scheduler();
            assert(nested_current.has_value());
            assert(nested_current->identity() == nested_ref.identity());
        }

        const auto restored = require_current_scheduler();
        assert(restored.has_value());
        assert(restored->identity() == ref.identity());
    }

    assert(!current_scheduler().has_value());

    std::vector<int> order;
    auto marker = std::make_unique<int>(3);
    scheduler.enqueue([&order] { order.push_back(1); });
    scheduler.enqueue([&order] { order.push_back(2); });
    scheduler.enqueue([moved = std::move(marker), &order] { order.push_back(*moved); });
    assert(marker == nullptr);
    assert(scheduler.ready_count() == 3);
    assert(scheduler.run_one());
    assert(order == std::vector<int>{1});
    assert(scheduler.run_until_idle() == 2);
    assert(order == std::vector<int>({1, 2, 3}));

    {
        current_scheduler_scope scope(ref);
        trampoline::schedule(ref, [&order, ref] {
            order.push_back(4);
            trampoline::schedule(ref, [&order] { order.push_back(5); });
        });
    }
    assert(scheduler.run_until_idle() >= 1);
    assert(order == std::vector<int>({1, 2, 3, 4, 5}));

    return 0;
}
