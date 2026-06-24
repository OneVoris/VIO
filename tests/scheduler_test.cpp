#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

#include "test_assert.hpp"
#include <functional>
#include <memory>
#include <vector>

namespace {

class inline_scheduler {
public:
    void enqueue(voris::io::continuation next) {
        if (next) {
            next();
        }
    }
};

} // namespace

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
        assert(trampoline::schedule(ref, [&order, ref] {
            order.push_back(4);
            assert(trampoline::schedule(ref, [&order] { order.push_back(5); }).has_value());
        }).has_value());
    }
    assert(scheduler.run_until_idle() >= 1);
    assert(order == std::vector<int>({1, 2, 3, 4, 5}));

    inline_scheduler inline_target;
    scheduler_ref inline_ref(inline_target);
    int count = 0;
    int current_depth = 0;
    int max_depth = 0;
    std::function<void()> chain;
    chain = [&] {
        ++current_depth;
        if (current_depth > max_depth) {
            max_depth = current_depth;
        }
        ++count;
        if (count < 128) {
            assert(trampoline::schedule(inline_ref, chain).has_value());
        }
        --current_depth;
    };

    assert(trampoline::schedule(inline_ref, chain).has_value());
    assert(count == 128);
    assert(max_depth == 1);

    return 0;
}
