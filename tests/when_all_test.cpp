#include <voris/io/when_all.hpp>

#include "test_assert.hpp"
#include <coroutine>
#include <memory>
#include <stdexcept>

namespace {

voris::io::task<int> value_task(int value) {
    co_return value;
}

voris::io::task<void> void_task() {
    co_return;
}

struct post_once {
    voris::io::scheduler_ref scheduler;

    struct operation_state {
        std::coroutine_handle<> continuation{};
        bool detached{false};
    };

    class awaiter {
    public:
        explicit awaiter(voris::io::scheduler_ref scheduler_ref)
            : scheduler_(scheduler_ref),
              state_(std::make_shared<operation_state>()) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : scheduler_(other.scheduler_),
              state_(std::move(other.state_)) {}

        awaiter& operator=(awaiter&& other) noexcept {
            if (this != &other) {
                detach();
                scheduler_ = other.scheduler_;
                state_ = std::move(other.state_);
            }
            return *this;
        }

        ~awaiter() {
            detach();
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> continuation) const {
            state_->continuation = continuation;
            auto scheduled = scheduler_.schedule([state = state_] {
                if (state->detached || !state->continuation) {
                    return;
                }

                auto continuation = state->continuation;
                state->continuation = {};
                continuation.resume();
            });
            assert(scheduled.has_value());
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

        voris::io::scheduler_ref scheduler_;
        std::shared_ptr<operation_state> state_;
    };

    awaiter operator co_await() const {
        return awaiter(scheduler);
    }
};

voris::io::task<int> scheduled_value(voris::io::scheduler_ref scheduler, int value) {
    co_await post_once{scheduler};
    co_return value;
}

voris::io::task<void> scheduled_void(voris::io::scheduler_ref scheduler) {
    co_await post_once{scheduler};
    co_return;
}

voris::io::task<int> scheduled_failure(voris::io::scheduler_ref scheduler) {
    co_await post_once{scheduler};
    throw std::runtime_error("when_all pending failure");
    co_return 0;
}

voris::io::task<int> mark_value_after_post(voris::io::scheduler_ref scheduler,
                                           int value,
                                           bool& resumed) {
    co_await post_once{scheduler};
    resumed = true;
    co_return value;
}

} // namespace

int main() {
    using namespace voris::io;

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scheduler_scope(ref);

    auto combined = when_all(value_task(1), void_task(), value_task(3));
    auto result = std::move(combined).take_result();
    assert(result.has_value());

    auto& tuple = *result;
    assert(std::get<0>(tuple).has_value());
    assert(*std::get<0>(tuple) == 1);
    assert(std::get<1>(tuple).has_value());
    assert(std::get<2>(tuple).has_value());
    assert(*std::get<2>(tuple) == 3);

    {
        auto child = scheduled_value(ref, 71);
        auto awaiter = std::move(child).operator co_await();
        assert(!awaiter.await_ready());
        assert(scheduler.run_one());

        const bool suspended = awaiter.await_suspend(std::noop_coroutine());
        assert(!suspended);

        auto completed_result = awaiter.await_resume();
        assert(completed_result.has_value());
        assert(*completed_result == 71);
    }

    {
        auto child = scheduled_void(ref);
        auto awaiter = std::move(child).operator co_await();
        assert(!awaiter.await_ready());
        assert(scheduler.run_one());

        const bool suspended = awaiter.await_suspend(std::noop_coroutine());
        assert(!suspended);

        auto completed_result = awaiter.await_resume();
        assert(completed_result.has_value());
    }

    {
        auto pending = when_all(scheduled_value(ref, 11), scheduled_value(ref, 22));
        assert(!pending.is_ready());
        assert(scheduler.run_until_idle() >= 2);
        assert(pending.is_ready());

        auto pending_result = std::move(pending).take_result();
        assert(pending_result.has_value());
        auto& pending_tuple = *pending_result;
        assert(std::get<0>(pending_tuple).has_value());
        assert(*std::get<0>(pending_tuple) == 11);
        assert(std::get<1>(pending_tuple).has_value());
        assert(*std::get<1>(pending_tuple) == 22);
    }

    {
        auto mixed = when_all(void_task(), scheduled_void(ref), scheduled_value(ref, 31));
        assert(!mixed.is_ready());
        assert(scheduler.run_until_idle() >= 2);
        assert(mixed.is_ready());

        auto mixed_result = std::move(mixed).take_result();
        assert(mixed_result.has_value());
        auto& mixed_tuple = *mixed_result;
        assert(std::get<0>(mixed_tuple).has_value());
        assert(std::get<1>(mixed_tuple).has_value());
        assert(std::get<2>(mixed_tuple).has_value());
        assert(*std::get<2>(mixed_tuple) == 31);
    }

    {
        auto failed = when_all(scheduled_failure(ref), scheduled_value(ref, 41));
        assert(!failed.is_ready());
        assert(scheduler.run_until_idle() >= 2);
        assert(failed.is_ready());

        auto failed_result = std::move(failed).take_result();
        assert(failed_result.has_value());
        auto& failed_tuple = *failed_result;
        assert(!std::get<0>(failed_tuple).has_value());
        assert(std::get<0>(failed_tuple).error().classification == vio_error_code::invalid_state);
        assert(std::get<1>(failed_tuple).has_value());
        assert(*std::get<1>(failed_tuple) == 41);
    }

    {
        bool first_resumed = false;
        bool second_resumed = false;
        {
            auto abandoned = when_all(mark_value_after_post(ref, 51, first_resumed),
                                      mark_value_after_post(ref, 52, second_resumed));
            assert(!abandoned.is_ready());
        }

        assert(scheduler.run_until_idle() >= 2);
        assert(!first_resumed);
        assert(!second_resumed);
    }

    {
        bool first_resumed = false;
        bool second_resumed = false;
        {
            auto first = mark_value_after_post(ref, 61, first_resumed);
            auto second = mark_value_after_post(ref, 62, second_resumed);
            auto abandoned_after_parent_queued = when_all(std::move(first), std::move(second));
            assert(!abandoned_after_parent_queued.is_ready());
            assert(scheduler.run_one());
            assert(first_resumed);
            assert(!second_resumed);
            assert(!abandoned_after_parent_queued.is_ready());
        }

        assert(scheduler.run_until_idle() >= 1);
        assert(first_resumed);
        assert(!second_resumed);
    }

    return 0;
}
