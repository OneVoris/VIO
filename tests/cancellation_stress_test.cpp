#include <voris/io/cancellation.hpp>
#include <voris/io/error.hpp>

#include "test_assert.hpp"
#include <atomic>
#include <barrier>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace {

constexpr int kDeterministicIterations = 1024;
constexpr int kRaceIterations = 512;

enum class terminal_kind {
    cancelled,
    backend_success,
    backend_failure,
};

struct terminal_outcome {
    terminal_kind kind{};
    std::optional<voris::io::cancellation_reason> cancellation{};
    voris::io::void_result result{};
};

// This helper is an executable/test-only ADR 0002 operation arbitration model.
// Move these scenarios into backend contract tests once the formal operation/backend surface exists.
struct operation_arbitration_state {
    explicit operation_arbitration_state(voris::io::cancellation_token token_value)
        : token(std::move(token_value)) {}

    voris::io::cancellation_token token;
    mutable std::mutex mutex;
    std::optional<terminal_outcome> terminal{};
    bool backend_reference_published{false};
    bool backend_reference_released{false};
    bool observer_attached{true};
    bool observed{false};
    int queued_count{0};
    int observed_count{0};
    int backend_release_count{0};
    int cancellation_callback_count{0};
};

[[nodiscard]] bool select_terminal_locked(operation_arbitration_state& state,
                                          terminal_outcome outcome) {
    if (state.terminal.has_value()) {
        return false;
    }

    state.terminal = std::move(outcome);
    if (state.observer_attached) {
        ++state.queued_count;
    }
    return true;
}

void on_cancellation(const std::shared_ptr<operation_arbitration_state>& state,
                     voris::io::cancellation_reason reason) {
    assert(!voris::io::detail::cancellation_internal_lock_held_for_testing(state->token));

    std::lock_guard guard(state->mutex);
    ++state->cancellation_callback_count;
    (void)select_terminal_locked(*state, terminal_outcome{
                                             .kind = terminal_kind::cancelled,
                                             .cancellation = reason,
                                             .result = std::unexpected(voris::io::make_error(
                                                 voris::io::vio_error_code::cancelled,
                                                 "operation cancelled")),
                                         });
}

[[nodiscard]] std::optional<terminal_outcome> terminal_for_testing(
    const std::shared_ptr<operation_arbitration_state>& state) {
    std::lock_guard guard(state->mutex);
    return state->terminal;
}

[[nodiscard]] int queued_count_for_testing(
    const std::shared_ptr<operation_arbitration_state>& state) {
    std::lock_guard guard(state->mutex);
    return state->queued_count;
}

[[nodiscard]] int cancellation_callback_count_for_testing(
    const std::shared_ptr<operation_arbitration_state>& state) {
    std::lock_guard guard(state->mutex);
    return state->cancellation_callback_count;
}

class operation_arbitration_model {
public:
    explicit operation_arbitration_model(voris::io::cancellation_token token)
        : state_(std::make_shared<operation_arbitration_state>(std::move(token))) {
        std::weak_ptr<operation_arbitration_state> weak_state = state_;
        registration_ = state_->token.register_callback(
            [weak_state](voris::io::cancellation_reason reason) {
                if (auto state = weak_state.lock()) {
                    on_cancellation(state, reason);
                }
            });
    }

    [[nodiscard]] bool submit() {
        std::lock_guard guard(state_->mutex);
        if (state_->terminal.has_value()) {
            return false;
        }

        state_->backend_reference_published = true;
        return true;
    }

    [[nodiscard]] bool backend_succeed() {
        return backend_complete(terminal_outcome{
            .kind = terminal_kind::backend_success,
            .cancellation = std::nullopt,
            .result = {},
        });
    }

    [[nodiscard]] bool backend_fail() {
        return backend_complete(terminal_outcome{
            .kind = terminal_kind::backend_failure,
            .cancellation = std::nullopt,
            .result = std::unexpected(voris::io::make_error(
                voris::io::vio_error_code::backend_failure, 5, "backend failure")),
        });
    }

    void detach_observer() {
        std::lock_guard guard(state_->mutex);
        state_->observer_attached = false;
    }

    [[nodiscard]] std::optional<terminal_outcome> terminal() const {
        return terminal_for_testing(state_);
    }

    [[nodiscard]] std::optional<terminal_outcome> observe_once() {
        std::lock_guard guard(state_->mutex);
        if (!state_->terminal.has_value() || !state_->observer_attached || state_->observed) {
            return std::nullopt;
        }

        state_->observed = true;
        ++state_->observed_count;
        return state_->terminal;
    }

    [[nodiscard]] int queued_count() const {
        return queued_count_for_testing(state_);
    }

    [[nodiscard]] int observed_count() const {
        std::lock_guard guard(state_->mutex);
        return state_->observed_count;
    }

    [[nodiscard]] int backend_release_count() const {
        std::lock_guard guard(state_->mutex);
        return state_->backend_release_count;
    }

    [[nodiscard]] int cancellation_callback_count() const {
        return cancellation_callback_count_for_testing(state_);
    }

    [[nodiscard]] std::shared_ptr<operation_arbitration_state> state_for_testing()
        const noexcept {
        return state_;
    }

private:
    [[nodiscard]] bool backend_complete(terminal_outcome outcome) {
        std::lock_guard guard(state_->mutex);
        if (!state_->backend_reference_published) {
            return false;
        }

        if (!state_->backend_reference_released) {
            state_->backend_reference_released = true;
            ++state_->backend_release_count;
        }

        return select_terminal_locked(*state_, std::move(outcome));
    }

    std::shared_ptr<operation_arbitration_state> state_;
    voris::io::cancellation_registration registration_;
};

void assert_cancelled_terminal(const terminal_outcome& terminal,
                               voris::io::cancellation_reason expected_reason) {
    assert(terminal.kind == terminal_kind::cancelled);
    assert(terminal.cancellation == expected_reason);
    assert(!terminal.result.has_value());
    assert(terminal.result.error().classification == voris::io::vio_error_code::cancelled);
}

void assert_backend_success_terminal(const terminal_outcome& terminal) {
    assert(terminal.kind == terminal_kind::backend_success);
    assert(!terminal.cancellation.has_value());
    assert(terminal.result.has_value());
}

void assert_backend_failure_terminal(const terminal_outcome& terminal) {
    assert(terminal.kind == terminal_kind::backend_failure);
    assert(!terminal.cancellation.has_value());
    assert(!terminal.result.has_value());
    assert(terminal.result.error().classification == voris::io::vio_error_code::backend_failure);
}

void stress_cancel_before_submit_keeps_cancelled_terminal() {
    using namespace voris::io;

    for (int i = 0; i != kDeterministicIterations; ++i) {
        cancellation_source source;
        operation_arbitration_model operation(source.token());

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(source.reason() == cancellation_reason::manual);
        assert(!source.request_cancellation(cancellation_reason::deadline));
        assert(source.reason() == cancellation_reason::manual);

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_cancelled_terminal(*terminal, cancellation_reason::manual);

        assert(!operation.submit());
        assert(!operation.backend_succeed());

        terminal = operation.terminal();
        assert(terminal.has_value());
        assert_cancelled_terminal(*terminal, cancellation_reason::manual);
        assert(operation.queued_count() == 1);
        assert(operation.backend_release_count() == 0);

        auto observed = operation.observe_once();
        assert(observed.has_value());
        assert_cancelled_terminal(*observed, cancellation_reason::manual);
        assert(!operation.observe_once().has_value());
        assert(operation.observed_count() == 1);
    }
}

void stress_active_cancel_then_backend_completion_keeps_cancelled_terminal() {
    using namespace voris::io;

    for (int i = 0; i != kDeterministicIterations; ++i) {
        cancellation_source source;
        operation_arbitration_model operation(source.token());

        assert(operation.submit());
        assert(source.request_cancellation(cancellation_reason::deadline));
        assert(!operation.backend_succeed());

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_cancelled_terminal(*terminal, cancellation_reason::deadline);
        assert(operation.queued_count() == 1);
        assert(operation.backend_release_count() == 1);
        assert(operation.cancellation_callback_count() == 1);
    }
}

void stress_active_backend_completion_then_cancel_keeps_backend_terminal() {
    using namespace voris::io;

    for (int i = 0; i != kDeterministicIterations; ++i) {
        cancellation_source source;
        operation_arbitration_model operation(source.token());

        assert(operation.submit());
        assert(operation.backend_succeed());
        assert(source.request_cancellation(cancellation_reason::runtime_shutdown));
        assert(source.reason() == cancellation_reason::runtime_shutdown);

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_backend_success_terminal(*terminal);
        assert(operation.queued_count() == 1);
        assert(operation.backend_release_count() == 1);
        assert(operation.cancellation_callback_count() == 1);
    }
}

void stress_active_cancel_races_backend_completion_once() {
    using namespace voris::io;

    std::atomic<int> cancelled_wins{0};
    std::atomic<int> backend_wins{0};

    for (int i = 0; i != kRaceIterations; ++i) {
        cancellation_source source;
        operation_arbitration_model operation(source.token());
        assert(operation.submit());

        std::barrier start(3);
        std::thread canceller([&] {
            start.arrive_and_wait();
            (void)source.request_cancellation(cancellation_reason::manual);
        });
        std::thread completer([&] {
            start.arrive_and_wait();
            (void)operation.backend_succeed();
        });

        start.arrive_and_wait();
        canceller.join();
        completer.join();

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        if (terminal->kind == terminal_kind::cancelled) {
            assert_cancelled_terminal(*terminal, cancellation_reason::manual);
            cancelled_wins.fetch_add(1);
        } else {
            assert_backend_success_terminal(*terminal);
            backend_wins.fetch_add(1);
        }

        assert(source.cancellation_requested());
        assert(source.reason() == cancellation_reason::manual);
        assert(operation.queued_count() == 1);
        assert(operation.backend_release_count() == 1);

        auto observed = operation.observe_once();
        assert(observed.has_value());
        assert(!operation.observe_once().has_value());
        assert(operation.observed_count() == 1);
    }

    assert(cancelled_wins.load() + backend_wins.load() == kRaceIterations);
}

void stress_late_cancel_after_success_or_failure_is_ignored() {
    using namespace voris::io;

    for (int i = 0; i != kDeterministicIterations; ++i) {
        {
            cancellation_source source;
            operation_arbitration_model operation(source.token());
            assert(operation.submit());
            assert(operation.backend_succeed());
            assert(source.request_cancellation(cancellation_reason::manual));

            auto terminal = operation.terminal();
            assert(terminal.has_value());
            assert_backend_success_terminal(*terminal);
            assert(operation.queued_count() == 1);
        }

        {
            cancellation_source source;
            operation_arbitration_model operation(source.token());
            assert(operation.submit());
            assert(operation.backend_fail());
            assert(source.request_cancellation(cancellation_reason::deadline));

            auto terminal = operation.terminal();
            assert(terminal.has_value());
            assert_backend_failure_terminal(*terminal);
            assert(operation.queued_count() == 1);
        }
    }
}

void stress_repeated_cancellation_retains_first_reason() {
    using namespace voris::io;

    for (int i = 0; i != kDeterministicIterations; ++i) {
        cancellation_source source;
        operation_arbitration_model operation(source.token());

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(!source.request_cancellation(cancellation_reason::deadline));
        assert(!source.request_cancellation(cancellation_reason::runtime_shutdown));
        assert(source.reason() == cancellation_reason::manual);

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_cancelled_terminal(*terminal, cancellation_reason::manual);
        assert(operation.cancellation_callback_count() == 1);
    }
}

void stress_detached_observer_does_not_observe_twice() {
    using namespace voris::io;

    for (int i = 0; i != kDeterministicIterations; ++i) {
        cancellation_source source;
        operation_arbitration_model operation(source.token());
        assert(operation.submit());

        operation.detach_observer();
        assert(source.request_cancellation(cancellation_reason::backend_abort));
        assert(!operation.backend_succeed());

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_cancelled_terminal(*terminal, cancellation_reason::backend_abort);
        assert(!operation.observe_once().has_value());
        assert(operation.queued_count() == 0);
        assert(operation.observed_count() == 0);
        assert(operation.backend_release_count() == 1);
    }
}

void cancellation_snapshot_after_model_destruction_keeps_callback_target_alive() {
    using namespace voris::io;

    cancellation_source source;
    std::barrier callback_snapshotted(2);
    std::barrier allow_callback_chain_to_continue(2);

    auto blocker = source.token().register_callback([&](cancellation_reason) {
        callback_snapshotted.arrive_and_wait();
        allow_callback_chain_to_continue.arrive_and_wait();
    });

    std::optional<operation_arbitration_model> operation(std::in_place, source.token());
    auto state = operation->state_for_testing();
    assert(operation->submit());

    std::thread canceller([&] {
        assert(source.request_cancellation(cancellation_reason::manual));
    });

    callback_snapshotted.arrive_and_wait();
    operation.reset();
    allow_callback_chain_to_continue.arrive_and_wait();
    canceller.join();

    auto terminal = terminal_for_testing(state);
    assert(terminal.has_value());
    assert_cancelled_terminal(*terminal, cancellation_reason::manual);
    assert(queued_count_for_testing(state) == 1);
    assert(cancellation_callback_count_for_testing(state) == 1);
}

} // namespace

int main() {
    using namespace voris::io;

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        cancellation_token token = source.token();
        int calls = 0;

        auto registration = token.register_callback([&](cancellation_reason reason) {
            ++calls;
            assert(reason == cancellation_reason::manual);
        });

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(!source.request_cancellation(cancellation_reason::deadline));
        assert(calls == 1);
        assert(!registration.active());
        assert(token.reason() == cancellation_reason::manual);
    }

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        cancellation_token token = source.token();
        auto registration = token.register_callback([](cancellation_reason) {
            assert(false);
        });
        registration.unregister();
        assert(source.request_cancellation(cancellation_reason::backend_abort));
        assert(token.reason() == cancellation_reason::backend_abort);
    }

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        cancellation_token token = source.token();
        std::atomic<int> calls{0};
        std::barrier start(3);

        auto registration = token.register_callback([&](cancellation_reason reason) {
            assert(reason == cancellation_reason::manual);
            assert(!detail::cancellation_internal_lock_held_for_testing(token));
            calls.fetch_add(1);
        });

        std::thread canceller([&] {
            start.arrive_and_wait();
            (void)source.request_cancellation(cancellation_reason::manual);
        });
        std::thread unregisterer([&] {
            start.arrive_and_wait();
            registration.unregister();
        });

        start.arrive_and_wait();
        canceller.join();
        unregisterer.join();

        assert(source.cancellation_requested());
        assert(source.reason() == cancellation_reason::manual);
        assert(!registration.active());
        assert(calls.load() <= 1);
    }

    stress_cancel_before_submit_keeps_cancelled_terminal();
    stress_active_cancel_then_backend_completion_keeps_cancelled_terminal();
    stress_active_backend_completion_then_cancel_keeps_backend_terminal();
    stress_active_cancel_races_backend_completion_once();
    stress_late_cancel_after_success_or_failure_is_ignored();
    stress_repeated_cancellation_retains_first_reason();
    stress_detached_observer_does_not_observe_twice();
    cancellation_snapshot_after_model_destruction_keeps_callback_target_alive();

    return 0;
}
