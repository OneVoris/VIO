#include <voris/io/when_any.hpp>
#include <voris/io/shard.hpp>
#include <voris/io/trampoline.hpp>

#include "test_assert.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace {

template<class Predicate>
bool run_until(voris::io::default_scheduler& scheduler,
               Predicate predicate,
               std::size_t max_steps = 32) {
    for (std::size_t step = 0; step < max_steps; ++step) {
        if (predicate()) {
            return true;
        }
        if (!scheduler.run_one()) {
            return predicate();
        }
    }
    return predicate();
}

class manual_gate {
    struct operation_state {
        std::coroutine_handle<> continuation{};
        bool ready{false};
        bool detached{false};
    };

public:
    explicit manual_gate(voris::io::scheduler_ref scheduler)
        : scheduler_(scheduler),
          state_(std::make_shared<operation_state>()) {}

    void complete() const {
        state_->ready = true;
        if (state_->detached || !state_->continuation) {
            return;
        }

        auto scheduled = scheduler_.schedule([state = state_] {
            if (state->detached || !state->continuation) {
                return;
            }

            auto continuation = state->continuation;
            state->continuation = {};
            continuation.resume();
        });
        assert(scheduled.has_value());
    }

    class awaiter {
    public:
        awaiter(voris::io::scheduler_ref scheduler,
                std::shared_ptr<operation_state> state) noexcept
            : scheduler_(scheduler),
              state_(std::move(state)) {}

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
            return state_->ready;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) const {
            if (state_->ready) {
                return false;
            }

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

        voris::io::scheduler_ref scheduler_;
        std::shared_ptr<operation_state> state_;
    };

    awaiter operator co_await() const {
        return awaiter(scheduler_, state_);
    }

private:
    voris::io::scheduler_ref scheduler_;
    std::shared_ptr<operation_state> state_;
};

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

        awaiter& operator=(awaiter&& other) noexcept {
            if (this != &other) {
                detach();
                state_ = std::move(other.state_);
            }
            return *this;
        }

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

class cancellation_wait {
    struct operation_state {
        explicit operation_state(voris::io::scheduler_ref scheduler_ref)
            : scheduler(scheduler_ref) {}

        voris::io::scheduler_ref scheduler;
        std::coroutine_handle<> continuation{};
        voris::io::cancellation_registration registration{};
        std::optional<voris::io::cancellation_reason> reason{};
        bool detached{false};
    };

public:
    cancellation_wait(voris::io::scheduler_ref scheduler, voris::io::cancellation_token token)
        : state_(std::make_shared<operation_state>(scheduler)),
          token_(std::move(token)) {}

    class awaiter {
    public:
        awaiter(std::shared_ptr<operation_state> state, voris::io::cancellation_token token)
            : state_(std::move(state)),
              token_(std::move(token)) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : state_(std::move(other.state_)),
              token_(std::move(other.token_)) {}

        awaiter& operator=(awaiter&& other) noexcept {
            if (this != &other) {
                detach();
                state_ = std::move(other.state_);
                token_ = std::move(other.token_);
            }
            return *this;
        }

        ~awaiter() {
            detach();
        }

        [[nodiscard]] bool await_ready() const {
            return token_.cancellation_requested();
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) const {
            state_->continuation = continuation;
            state_->registration = token_.register_callback([state = state_](
                                                                voris::io::cancellation_reason reason) {
                state->reason = reason;
                auto scheduled = state->scheduler.schedule([state] {
                    if (state->detached || !state->continuation) {
                        return;
                    }

                    auto continuation = state->continuation;
                    state->continuation = {};
                    continuation.resume();
                });
                assert(scheduled.has_value());
            });
            return true;
        }

        voris::io::cancellation_reason await_resume() const {
            return token_.reason().value_or(voris::io::cancellation_reason::manual);
        }

    private:
        void detach() noexcept {
            if (state_ != nullptr) {
                state_->continuation = {};
                state_->detached = true;
                state_->registration.unregister();
            }
        }

        std::shared_ptr<operation_state> state_;
        voris::io::cancellation_token token_;
    };

    awaiter operator co_await() const {
        return awaiter(state_, token_);
    }

private:
    std::shared_ptr<operation_state> state_;
    voris::io::cancellation_token token_;
};

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

voris::io::task<int> value_task(int value) {
    co_return value;
}

voris::io::task<int> gated_value(manual_gate& gate, int value) {
    co_await gate;
    co_return value;
}

voris::io::task<int> inline_gated_value(inline_gate& gate, int value) {
    co_await gate;
    co_return value;
}

int run_when_any_system_lane_exhaustion_child() {
    using namespace voris::io;

    std::set_terminate([] { std::_Exit(42); });

    shard saturated(1);
    cancellation_source losers;
    auto state = std::make_shared<detail::when_any_state<io_result<int>, io_result<int>>>(
        losers);
    current_scheduler_scope scope(saturated.scheduler());
    assert(state->install_parent(std::noop_coroutine(), saturated.scheduler()));
    assert(trampoline::schedule_system(saturated.scheduler(), [] {}).has_value());

    state->publish<0>(io_result<int>{71});
    return 0;
}

int run_child_process(char const* executable, char const* mode) {
    std::string command = "\"";
    command += executable;
    command += "\" ";
    command += mode;
#ifdef _WIN32
    command += " >NUL 2>NUL";
#else
    command += " >/dev/null 2>/dev/null";
#endif
    return std::system(command.c_str());
}

voris::io::task<void> gated_void(manual_gate& gate) {
    co_await gate;
    co_return;
}

voris::io::task<int> gated_error(manual_gate& gate) {
    co_await gate;
    co_return std::unexpected(voris::io::make_error(voris::io::vio_error_code::backend_failure,
                                                    "winner failed"));
}

voris::io::task<int> cancellation_observer(voris::io::scheduler_ref scheduler,
                                           voris::io::cancellation_token token,
                                           bool& observed) {
    auto reason = co_await cancellation_wait{scheduler, std::move(token)};
    observed = reason == voris::io::cancellation_reason::manual;
    co_return -1;
}

voris::io::task<int> mark_value_after_post(voris::io::scheduler_ref scheduler, bool& resumed) {
    co_await post_once{scheduler};
    resumed = true;
    co_return 1;
}

voris::io::task<void> mark_after_when_any(voris::io::cancellation_source& losers,
                                          manual_gate& winner_gate,
                                          manual_gate& loser_gate,
                                          bool& resumed) {
    auto result = co_await voris::io::when_any(losers,
                                               gated_value(winner_gate, 81),
                                               gated_value(loser_gate, 82));
    (void)result;
    resumed = true;
    co_return;
}

} // namespace

int main(int argc, char** argv) {
    using namespace voris::io;

    if (argc == 2 && std::string(argv[1]) == "--when-any-system-lane-exhaustion") {
        return run_when_any_system_lane_exhaustion_child();
    }

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scheduler_scope(ref);

    {
        cancellation_source losers;
        auto any = when_any(losers, value_task(10), value_task(20));
        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 0);
        assert(std::get<0>(result->result).has_value());
        assert(*std::get<0>(result->result) == 10);
        assert(losers.cancellation_requested());
        assert(losers.reason() == cancellation_reason::manual);
    }

    {
        manual_gate first_gate(ref);
        manual_gate second_gate(ref);
        cancellation_source losers;
        auto any = when_any(losers, gated_value(first_gate, 11), gated_value(second_gate, 22));
        assert(!any.is_ready());

        second_gate.complete();
        assert(scheduler.run_one());
        assert(!any.is_ready());
        assert(scheduler.run_one());
        assert(!any.is_ready());
        assert(scheduler.run_one());
        assert(losers.cancellation_requested());
        assert(!any.is_ready());

        first_gate.complete();
        assert(scheduler.run_until_idle() >= 3);
        assert(any.is_ready());

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 1);
        assert(std::get<1>(result->result).has_value());
        assert(*std::get<1>(result->result) == 22);
    }

    {
        manual_gate winner_gate(ref);
        manual_gate loser_gate(ref);
        cancellation_source losers;
        auto any = when_any(losers, gated_value(winner_gate, 21), gated_value(loser_gate, 22));
        assert(!any.is_ready());

        winner_gate.complete();
        assert(run_until(scheduler, [&] { return losers.cancellation_requested(); }, 2));
        assert(losers.reason() == cancellation_reason::manual);
        assert(!any.is_ready());

        loser_gate.complete();
        assert(run_until(scheduler, [&] { return any.is_ready(); }));

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 0);
        assert(std::get<0>(result->result).has_value());
        assert(*std::get<0>(result->result) == 21);
    }

    {
        manual_gate winner_gate(ref);
        manual_gate loser_gate(ref);
        cancellation_token token;
        task<when_any_result<io_result<int>, io_result<int>>> any;
        {
            cancellation_source scoped_losers;
            token = scoped_losers.token();
            any = when_any(scoped_losers,
                           gated_value(winner_gate, 23),
                           gated_value(loser_gate, 24));
        }

        assert(!any.is_ready());
        winner_gate.complete();
        assert(run_until(scheduler, [&] { return token.cancellation_requested(); }));
        assert(token.reason() == cancellation_reason::manual);
        assert(!any.is_ready());

        loser_gate.complete();
        assert(run_until(scheduler, [&] { return any.is_ready(); }));

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 0);
        assert(std::get<0>(result->result).has_value());
        assert(*std::get<0>(result->result) == 23);
    }

    {
        manual_gate winner_gate(ref);
        cancellation_source losers;
        bool loser_observed_cancellation = false;
        auto any = when_any(losers,
                            cancellation_observer(ref, losers.token(), loser_observed_cancellation),
                            gated_value(winner_gate, 31));
        assert(!any.is_ready());

        winner_gate.complete();
        assert(scheduler.run_one());
        assert(scheduler.run_one());
        assert(!loser_observed_cancellation);
        assert(scheduler.run_one());
        assert(losers.cancellation_requested());
        assert(losers.reason() == cancellation_reason::manual);
        assert(!any.is_ready());

        assert(scheduler.run_one());
        assert(loser_observed_cancellation);
        assert(!any.is_ready());
        assert(scheduler.run_until_idle() >= 2);
        assert(any.is_ready());

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 1);
        assert(std::get<1>(result->result).has_value());
        assert(*std::get<1>(result->result) == 31);
    }

    {
        manual_gate winner_gate(ref);
        manual_gate loser_gate(ref);
        cancellation_source losers;
        auto any = when_any(losers, gated_error(winner_gate), gated_value(loser_gate, 44));
        assert(!any.is_ready());

        winner_gate.complete();
        assert(scheduler.run_one());
        assert(scheduler.run_one());
        assert(scheduler.run_one());
        assert(losers.cancellation_requested());
        assert(!any.is_ready());

        loser_gate.complete();
        assert(scheduler.run_until_idle() >= 3);
        assert(any.is_ready());

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 0);
        assert(!std::get<0>(result->result).has_value());
        assert(std::get<0>(result->result).error().classification ==
               vio_error_code::backend_failure);
    }

    {
        manual_gate void_gate(ref);
        manual_gate int_gate(ref);
        cancellation_source losers;
        auto any = when_any(losers, gated_void(void_gate), gated_value(int_gate, 55));
        assert(!any.is_ready());

        void_gate.complete();
        assert(scheduler.run_one());
        assert(scheduler.run_one());
        assert(scheduler.run_one());
        assert(losers.cancellation_requested());
        assert(!any.is_ready());

        int_gate.complete();
        assert(scheduler.run_until_idle() >= 3);
        assert(any.is_ready());

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 0);
        assert(std::get<0>(result->result).has_value());
    }

    {
        bool first_resumed = false;
        bool second_resumed = false;
        cancellation_source losers;
        {
            auto abandoned = when_any(losers,
                                      mark_value_after_post(ref, first_resumed),
                                      mark_value_after_post(ref, second_resumed));
            assert(!abandoned.is_ready());
        }

        assert(scheduler.run_until_idle() >= 2);
        assert(!first_resumed);
        assert(!second_resumed);
    }

    {
        manual_gate winner_gate(ref);
        manual_gate loser_gate(ref);
        cancellation_source losers;
        bool parent_resumed = false;
        {
            auto parent = mark_after_when_any(losers, winner_gate, loser_gate, parent_resumed);
            assert(!parent.is_ready());

            winner_gate.complete();
            assert(run_until(scheduler, [&] { return losers.cancellation_requested(); }, 2));
            assert(!parent.is_ready());
        }

        assert(scheduler.run_until_idle() >= 1);
        assert(!parent_resumed);
    }

    {
        shard saturated(1);
        inline_gate winner_gate;
        inline_gate loser_gate;
        cancellation_source losers;
        current_scheduler_scope shard_scope(saturated.scheduler());
        auto any = when_any(losers,
                            inline_gated_value(winner_gate, 61),
                            inline_gated_value(loser_gate, 62));
        assert(!any.is_ready());
        assert(saturated.submit([] {}).has_value());

        winner_gate.complete();
        assert(!any.is_ready());
        assert(saturated.drain() >= 1);
        assert(losers.cancellation_requested());
        assert(!any.is_ready());

        loser_gate.complete();
        assert(saturated.drain() >= 1);
        assert(any.is_ready());

        auto result = std::move(any).take_result();
        assert(result.has_value());
        assert(result->index == 0);
        assert(std::get<0>(result->result).has_value());
        assert(*std::get<0>(result->result) == 61);
    }

    {
        assert(run_child_process(argv[0], "--when-any-system-lane-exhaustion") != 0);
    }

    {
        cancellation_source losers;
        auto state = std::make_shared<detail::when_any_state<io_result<int>, io_result<int>>>(
            losers);
        detail::when_any_winner_awaiter<io_result<int>, io_result<int>> awaiter(state);
        auto previous_scheduler = current_scheduler();

        set_current_scheduler_for_testing(std::nullopt);
        assert(!awaiter.await_ready());
        assert(!awaiter.await_suspend(std::noop_coroutine()));
        auto result = awaiter.await_resume();
        set_current_scheduler_for_testing(previous_scheduler);

        assert(result.index == 0);
        assert(!std::get<0>(result.result).has_value());
        assert(std::get<0>(result.result).error().classification == vio_error_code::invalid_state);
        assert(losers.cancellation_requested());
        assert(losers.reason() == cancellation_reason::manual);
    }

    return 0;
}
