#include <voris/io/async_scope.hpp>

#include "test_assert.hpp"
#include <coroutine>
#include <memory>
#include <stdexcept>

namespace {

voris::io::task<int> ok_task() {
    co_return 7;
}

voris::io::task<int> failing_task() {
    throw std::runtime_error("scope failure");
    co_return 0;
}

voris::io::task<void> ok_void_task() {
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

voris::io::task<void> scheduled_failure(voris::io::scheduler_ref scheduler) {
    co_await post_once{scheduler};
    throw std::runtime_error("scope pending failure");
    co_return;
}

voris::io::task<void> observe_stop_after_resume(voris::io::cancellation_token token,
                                                voris::io::scheduler_ref scheduler,
                                                bool& resumed,
                                                bool& saw_stop) {
    co_await post_once{scheduler};
    resumed = true;
    saw_stop = token.cancellation_requested();
    co_return;
}

voris::io::task<void> mark_resume_after_post(voris::io::scheduler_ref scheduler, bool& resumed) {
    co_await post_once{scheduler};
    resumed = true;
    co_return;
}

} // namespace

int main() {
    using namespace voris::io;

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scheduler_scope(ref);

    background_error_sink sink;
    async_scope scope(&sink);

    assert(scope.pending_count() == 0);
    assert(scope.spawn(ok_task()).has_value());
    assert(scope.spawn(ok_void_task()).has_value());
    assert(scope.join().has_value());
    assert(scope.errors().empty());
    assert(sink.empty());

    assert(scope.spawn(scheduled_value(ref, 11)).has_value());
    assert(scope.pending_count() == 1);
    auto pending_join = scope.join();
    assert(!pending_join.has_value());
    assert(pending_join.error().classification == vio_error_code::invalid_state);
    assert(scope.pending_count() == 1);
    assert(scheduler.run_until_idle() >= 1);
    assert(scope.join().has_value());
    assert(scope.pending_count() == 0);

    {
        background_error_sink pending_sink;
        async_scope pending_scope(&pending_sink);

        assert(pending_scope.spawn(scheduled_failure(ref)).has_value());
        assert(pending_scope.pending_count() == 1);
        assert(scheduler.run_until_idle() >= 1);

        auto pending_failure = pending_scope.join();
        assert(!pending_failure.has_value());
        assert(pending_failure.error().classification == vio_error_code::invalid_state);
        assert(pending_scope.pending_count() == 0);
        assert(pending_scope.errors().size() == 1);
        assert(pending_sink.size() == 1);
    }

    {
        background_error_sink stop_sink;
        async_scope stopping_scope(&stop_sink);
        bool resumed = false;
        bool saw_stop = false;

        assert(stopping_scope
                   .spawn(observe_stop_after_resume(stopping_scope.token(), ref, resumed, saw_stop))
                   .has_value());
        assert(stopping_scope.pending_count() == 1);
        assert(stopping_scope.request_stop(cancellation_reason::manual));
        assert(stopping_scope.stop_requested());
        assert(stopping_scope.token().reason() == cancellation_reason::manual);
        assert(!stopping_scope.request_stop(cancellation_reason::runtime_shutdown));
        assert(stopping_scope.token().reason() == cancellation_reason::manual);

        auto rejected_during_stop = stopping_scope.spawn(ok_void_task());
        assert(!rejected_during_stop.has_value());
        assert(rejected_during_stop.error().classification == vio_error_code::cancelled);
        assert(stopping_scope.pending_count() == 1);

        assert(scheduler.run_until_idle() >= 1);
        assert(resumed);
        assert(saw_stop);
        assert(stopping_scope.join().has_value());
        assert(stopping_scope.pending_count() == 0);
    }

    {
        bool resumed_after_scope_destroy = false;
        {
            async_scope abandoned_scope;
            assert(abandoned_scope.spawn(mark_resume_after_post(ref, resumed_after_scope_destroy))
                       .has_value());
            assert(abandoned_scope.pending_count() == 1);
        }

        assert(scheduler.run_until_idle() >= 1);
        assert(!resumed_after_scope_destroy);
    }

    {
        async_scope stopped_scope;
        assert(stopped_scope.request_stop(cancellation_reason::manual));

        bool resumed_after_reject = false;
        auto rejected_pending =
            stopped_scope.spawn(mark_resume_after_post(ref, resumed_after_reject));
        assert(!rejected_pending.has_value());
        assert(rejected_pending.error().classification == vio_error_code::cancelled);

        assert(scheduler.run_until_idle() >= 1);
        assert(!resumed_after_reject);
    }

    auto failure = scope.spawn(failing_task());
    assert(!failure.has_value());
    assert(failure.error().classification == vio_error_code::invalid_state);
    assert(scope.errors().size() == 1);
    assert(sink.size() == 1);

    auto second_failure = scope.spawn(failing_task());
    assert(!second_failure.has_value());
    assert(second_failure.error().classification == vio_error_code::invalid_state);
    assert(scope.errors().size() == 2);
    assert(sink.size() == 2);
    assert(!scope.join().has_value());

    assert(scope.request_stop());
    assert(scope.stop_requested());
    assert(scope.token().reason() == cancellation_reason::scope_shutdown);
    assert(!scope.request_stop(cancellation_reason::runtime_shutdown));

    auto rejected = scope.spawn(ok_task());
    assert(!rejected.has_value());
    assert(rejected.error().classification == vio_error_code::cancelled);

    return 0;
}
