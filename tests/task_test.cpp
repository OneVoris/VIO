#include <voris/io/task.hpp>

#include <cassert>
#include <stdexcept>

namespace {

voris::io::task<int> value_task(int value) {
    co_return value;
}

voris::io::task<void> void_task(bool& entered) {
    entered = true;
    co_return;
}

voris::io::task<int> throwing_task() {
    throw std::runtime_error("boom");
    co_return 0;
}

struct post_once {
    voris::io::scheduler_ref scheduler;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) const {
        (void)scheduler.schedule([continuation] { continuation.resume(); });
    }

    void await_resume() const noexcept {}
};

voris::io::task<int> scheduled_value(voris::io::scheduler_ref scheduler, int value) {
    co_await post_once{scheduler};
    co_return value;
}

voris::io::task<int> awaiting_parent(voris::io::scheduler_ref scheduler) {
    auto child = co_await scheduled_value(scheduler, 41);
    if (!child.has_value()) {
        co_return child;
    }
    co_return *child + 1;
}

} // namespace

int main() {
    using namespace voris::io;

    set_current_scheduler_for_testing(std::nullopt);
    bool missing_scheduler_body_entered = false;
    auto missing_scheduler_task = void_task(missing_scheduler_body_entered);
    assert(!missing_scheduler_body_entered);
    auto missing_scheduler_result = std::move(missing_scheduler_task).take_result();
    assert(!missing_scheduler_result.has_value());
    assert(missing_scheduler_result.error().classification == vio_error_code::invalid_state);

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);

    {
        current_scheduler_scope scope(ref);

        auto value = value_task(7);
        assert(value.is_ready());
        auto value_result = std::move(value).take_result();
        assert(value_result.has_value());
        assert(*value_result == 7);

        bool entered = false;
        auto void_success = void_task(entered);
        assert(entered);
        auto void_result = std::move(void_success).take_result();
        assert(void_result.has_value());

        auto throwing = throwing_task();
        auto throwing_result = std::move(throwing).take_result();
        assert(!throwing_result.has_value());
        assert(throwing_result.error().classification == vio_error_code::invalid_state);
    }

    task<int> empty;
    auto empty_result = std::move(empty).take_result();
    assert(!empty_result.has_value());
    assert(empty_result.error().classification == vio_error_code::invalid_state);

    {
        current_scheduler_scope scope(ref);
        auto moved_from = value_task(9);
        auto moved_to = std::move(moved_from);
        auto moved_from_result = std::move(moved_from).take_result();
        assert(!moved_from_result.has_value());
        assert(moved_from_result.error().classification == vio_error_code::invalid_state);
        auto moved_to_result = std::move(moved_to).take_result();
        assert(moved_to_result.has_value());
        assert(*moved_to_result == 9);
    }

    {
        current_scheduler_scope scope(ref);
        auto parent = awaiting_parent(ref);
        assert(!parent.is_ready());
        assert(scheduler.run_until_idle() >= 1);
        assert(parent.is_ready());
        auto parent_result = std::move(parent).take_result();
        assert(parent_result.has_value());
        assert(*parent_result == 42);
    }

    return 0;
}
