#include <voris/io/cancellation.hpp>
#include <voris/io/error.hpp>

#include "test_assert.hpp"
#include <atomic>
#include <barrier>
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

class operation_arbitration_model {
public:
    explicit operation_arbitration_model(voris::io::cancellation_token token)
        : token_(std::move(token)) {
        registration_ = token_.register_callback(
            [this](voris::io::cancellation_reason reason) { on_cancellation(reason); });
    }

    [[nodiscard]] bool submit() {
        std::lock_guard guard(mutex_);
        if (terminal_.has_value()) {
            return false;
        }

        backend_reference_published_ = true;
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
        std::lock_guard guard(mutex_);
        observer_attached_ = false;
    }

    [[nodiscard]] std::optional<terminal_outcome> terminal() const {
        std::lock_guard guard(mutex_);
        return terminal_;
    }

    [[nodiscard]] std::optional<terminal_outcome> observe_once() {
        std::lock_guard guard(mutex_);
        if (!terminal_.has_value() || !observer_attached_ || observed_) {
            return std::nullopt;
        }

        observed_ = true;
        ++observed_count_;
        return terminal_;
    }

    [[nodiscard]] int queued_count() const {
        std::lock_guard guard(mutex_);
        return queued_count_;
    }

    [[nodiscard]] int observed_count() const {
        std::lock_guard guard(mutex_);
        return observed_count_;
    }

    [[nodiscard]] int backend_release_count() const {
        std::lock_guard guard(mutex_);
        return backend_release_count_;
    }

    [[nodiscard]] int cancellation_callback_count() const noexcept {
        return cancellation_callback_count_.load();
    }

private:
    void on_cancellation(voris::io::cancellation_reason reason) {
        assert(!voris::io::detail::cancellation_internal_lock_held_for_testing(token_));
        cancellation_callback_count_.fetch_add(1);

        std::lock_guard guard(mutex_);
        (void)select_terminal_locked(terminal_outcome{
            .kind = terminal_kind::cancelled,
            .cancellation = reason,
            .result = std::unexpected(voris::io::make_error(
                voris::io::vio_error_code::cancelled, "operation cancelled")),
        });
    }

    [[nodiscard]] bool backend_complete(terminal_outcome outcome) {
        std::lock_guard guard(mutex_);
        if (!backend_reference_published_) {
            return false;
        }

        if (!backend_reference_released_) {
            backend_reference_released_ = true;
            ++backend_release_count_;
        }

        return select_terminal_locked(std::move(outcome));
    }

    [[nodiscard]] bool select_terminal_locked(terminal_outcome outcome) {
        if (terminal_.has_value()) {
            return false;
        }

        terminal_ = std::move(outcome);
        if (observer_attached_) {
            ++queued_count_;
        }
        return true;
    }

    voris::io::cancellation_token token_;
    voris::io::cancellation_registration registration_;
    mutable std::mutex mutex_;
    std::optional<terminal_outcome> terminal_{};
    bool backend_reference_published_{false};
    bool backend_reference_released_{false};
    bool observer_attached_{true};
    bool observed_{false};
    int queued_count_{0};
    int observed_count_{0};
    int backend_release_count_{0};
    std::atomic<int> cancellation_callback_count_{0};
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

    return 0;
}
