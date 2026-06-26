#include <voris/io/scheduler.hpp>
#include <voris/io/trampoline.hpp>

#include "test_assert.hpp"
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

namespace {

int function_pointer_calls = 0;

void record_function_pointer_call() {
    ++function_pointer_calls;
}

struct member_probe {
    int data{42};

    [[nodiscard]] int value() const {
        return data;
    }
};

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

    static_assert(std::movable<continuation>);
    static_assert(!std::copy_constructible<continuation>);
    static_assert(!std::is_copy_assignable_v<continuation>);
#if defined(__cpp_lib_move_only_function)
    static_assert(!std::is_same_v<continuation, std::move_only_function<void()>>);
#endif

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
    continuation empty;
    assert(!empty);

    using function_pointer = void (*)();
    function_pointer missing_function = nullptr;
    continuation null_function(missing_function);
    assert(!null_function);

    function_pointer present_function = &record_function_pointer_call;
    continuation function_callback(present_function);
    assert(function_callback);
    function_callback();
    assert(function_pointer_calls == 1);

    member_probe probe;
    using member_function_pointer = int (member_probe::*)() const;
    member_function_pointer missing_member_function = nullptr;
    detail::move_only_function<int(const member_probe&)> null_member_function(
        missing_member_function);
    assert(!null_member_function);

    member_function_pointer present_member_function = &member_probe::value;
    detail::move_only_function<int(const member_probe&)> member_function_callback(
        present_member_function);
    assert(member_function_callback);
    assert(member_function_callback(probe) == 42);

    using member_data_pointer = int member_probe::*;
    member_data_pointer missing_member_data = nullptr;
    detail::move_only_function<int&(member_probe&)> null_member_data(missing_member_data);
    assert(!null_member_data);

    member_data_pointer present_member_data = &member_probe::data;
    detail::move_only_function<int&(member_probe&)> member_data_callback(present_member_data);
    assert(member_data_callback);
    assert(&member_data_callback(probe) == &probe.data);

    bool direct_called = false;
    auto direct_marker = std::make_unique<int>(7);
    continuation direct(
        [moved = std::move(direct_marker), &direct_called, &order] mutable {
            direct_called = true;
            order.push_back(*moved);
        });
    assert(direct_marker == nullptr);
    assert(direct);
    continuation moved_direct(std::move(direct));
    assert(moved_direct);
    moved_direct();
    assert(direct_called);
    assert(order == std::vector<int>({7}));

    auto marker = std::make_unique<int>(3);
    scheduler.enqueue([&order] { order.push_back(1); });
    scheduler.enqueue([&order] { order.push_back(2); });
    scheduler.enqueue([moved = std::move(marker), &order] { order.push_back(*moved); });
    assert(marker == nullptr);
    assert(scheduler.ready_count() == 3);
    assert(scheduler.run_one());
    assert(order == std::vector<int>({7, 1}));
    assert(scheduler.run_until_idle() == 2);
    assert(order == std::vector<int>({7, 1, 2, 3}));

    {
        current_scheduler_scope scope(ref);
        assert(trampoline::schedule(ref, [&order, ref] {
            order.push_back(4);
            assert(trampoline::schedule(ref, [&order] { order.push_back(5); }).has_value());
        }).has_value());
    }
    assert(scheduler.run_until_idle() >= 1);
    assert(order == std::vector<int>({7, 1, 2, 3, 4, 5}));

    assert(ref.schedule_system([&order] { order.push_back(6); }).has_value());
    assert(scheduler.run_one());
    assert(order == std::vector<int>({7, 1, 2, 3, 4, 5, 6}));

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
