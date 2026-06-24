#include <voris/io/task.hpp>
#include <voris/io/shard.hpp>
#include <voris/io/trampoline.hpp>

#include "test_assert.hpp"
#include <coroutine>
#include <memory>
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

class inline_gate {
    struct operation_state {
        std::coroutine_handle<> continuation{};
        bool ready{false};
        bool detached{false};
    };

public:
    inline_gate()
        : state_(std::make_shared<operation_state>()) {}

    void complete() const {
        state_->ready = true;
        if (state_->detached || !state_->continuation) {
            return;
        }

        auto continuation = state_->continuation;
        state_->continuation = {};
        continuation.resume();
    }

    class awaiter {
    public:
        explicit awaiter(std::shared_ptr<operation_state> state) noexcept
            : state_(std::move(state)) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : state_(std::move(other.state_)) {}

        ~awaiter() {
            detach();
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return state_->ready;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) const {
            state_->continuation = continuation;
            return true;
        }

        void await_resume() const noexcept {}

    private:
        void detach() noexcept {
            if (state_ != nullptr) {
                state_->continuation = {};
                state_->detached = true;
            }
        }

        std::shared_ptr<operation_state> state_;
    };

    awaiter operator co_await() const {
        return awaiter(state_);
    }

private:
    std::shared_ptr<operation_state> state_;
};

voris::io::task<int> gated_value(inline_gate& gate, int value) {
    co_await gate;
    co_return value;
}

voris::io::task<int> awaiting_gated_child(inline_gate& gate) {
    auto child = co_await gated_value(gate, 41);
    if (!child.has_value()) {
        co_return child;
    }
    co_return *child + 1;
}

voris::io::task<void> repeated_await_attempt(voris::io::io_result<int>& first,
                                             voris::io::io_result<int>& second) {
    auto child = value_task(5);
    first = co_await std::move(child);
    second = co_await std::move(child);
    co_return;
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

    {
        shard saturated(1);
        inline_gate gate;
        current_scheduler_scope scope(saturated.scheduler());
        auto parent = awaiting_gated_child(gate);
        assert(!parent.is_ready());
        assert(saturated.submit([] {}).has_value());

        gate.complete();
        assert(!parent.is_ready());
        assert(saturated.drain() >= 1);
        assert(parent.is_ready());

        auto parent_result = std::move(parent).take_result();
        assert(parent_result.has_value());
        assert(*parent_result == 42);
    }

    {
        shard saturated(1);
        inline_gate gate;
        current_scheduler_scope scope(saturated.scheduler());
        auto parent = awaiting_gated_child(gate);
        assert(!parent.is_ready());
        assert(trampoline::schedule_system(saturated.scheduler(), [] {}).has_value());

        gate.complete();
        assert(parent.is_ready());

        auto parent_result = std::move(parent).take_result();
        assert(parent_result.has_value());
        assert(*parent_result == 42);
        assert(saturated.drain() == 1);
    }

    {
        current_scheduler_scope scope(ref);
        io_result<int> first = std::unexpected(make_error(vio_error_code::invalid_state));
        io_result<int> second = std::unexpected(make_error(vio_error_code::invalid_state));
        auto repeated = repeated_await_attempt(first, second);
        auto repeated_result = std::move(repeated).take_result();
        assert(repeated_result.has_value());
        assert(first.has_value());
        assert(*first == 5);
        assert(!second.has_value());
        assert(second.error().classification == vio_error_code::invalid_state);
    }

    return 0;
}
