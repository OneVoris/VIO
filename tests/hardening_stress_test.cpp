#include <voris/io/backend.hpp>
#include <voris/io/cancellation.hpp>
#include <voris/io/runtime.hpp>

#include "test_assert.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

using clock_type = std::chrono::steady_clock;

struct stress_config {
    bool long_mode{false};
    std::size_t iterations{128};
    std::chrono::seconds duration{0};
};

struct stress_counter {
    std::size_t iterations{};
    std::chrono::milliseconds elapsed{};
};

[[nodiscard]] std::optional<std::string> read_env_string(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t length = 0;
    const auto result = _dupenv_s(&buffer, &length, name);
    assert(result == 0);
    if (buffer == nullptr) {
        return std::nullopt;
    }

    std::string value(buffer, length == 0 ? 0 : length - 1);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

[[noreturn]] void fail_invalid_env(const char* name,
                                   const std::string& value,
                                   const char* reason) {
    std::fprintf(stderr,
                 "invalid %s=%s: %s; expected unsigned decimal digits only\n",
                 name,
                 value.c_str(),
                 reason);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

[[nodiscard]] std::optional<std::size_t> read_size_env(const char* name) {
    const auto value = read_env_string(name);
    if (!value.has_value()) {
        return std::nullopt;
    }

    if (value->front() == '+' || value->front() == '-') {
        fail_invalid_env(name, *value, "leading sign is not allowed");
    }
    if (!std::ranges::all_of(*value, [](char current) {
            return current >= '0' && current <= '9';
        })) {
        fail_invalid_env(name, *value, "non-decimal character is not allowed");
    }

    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(value->c_str(), &end, 10);
    if (errno != 0 || end != value->c_str() + value->size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        fail_invalid_env(name, *value, "value is out of range");
    }
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] stress_config load_config() {
    stress_config config{};
    if (const auto mode = read_env_string("VIO_HARDENING_STRESS_MODE")) {
        if (*mode == "long") {
            config.long_mode = true;
        } else {
            assert(*mode == "quick");
        }
    }

    config.iterations = read_size_env("VIO_HARDENING_STRESS_ITERATIONS")
                            .value_or(config.long_mode ? 1024U : 128U);
    assert(config.iterations > 0);

    const auto seconds = read_size_env("VIO_HARDENING_STRESS_SECONDS")
                             .value_or(config.long_mode ? 60U : 0U);
    config.duration = std::chrono::seconds(seconds);
    return config;
}

template<class Function>
[[nodiscard]] stress_counter run_for_budget(std::size_t minimum_iterations,
                                            std::chrono::seconds duration,
                                            Function function) {
    const auto started = clock_type::now();
    const auto deadline = started + duration;
    std::size_t iterations = 0;
    do {
        function(iterations);
        ++iterations;
    } while (iterations < minimum_iterations ||
             (duration.count() > 0 && clock_type::now() < deadline));

    return stress_counter{
        .iterations = iterations,
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_type::now() - started),
    };
}

enum class race_terminal_kind {
    cancelled,
    closed,
    runtime_shutdown,
    backend_success,
};

struct race_handle_token {
    std::size_t native_handle{};
    std::size_t generation{};

    [[nodiscard]] friend bool operator==(race_handle_token lhs,
                                         race_handle_token rhs) noexcept = default;
};

struct race_terminal {
    race_terminal_kind kind{};
    std::optional<voris::io::cancellation_reason> cancellation{};
    voris::io::void_result result{};
};

struct race_operation_state {
    mutable std::mutex mutex;
    race_handle_token current{1, 1};
    bool handle_open{true};
    bool submitted{false};
    bool backend_reference_released{false};
    bool observer_attached{true};
    bool observed{false};
    bool stopped{false};
    std::optional<race_terminal> terminal{};
    std::size_t queued_count{0};
    std::size_t observed_count{0};
    std::size_t backend_release_count{0};
    std::size_t stale_event_count{0};
    std::size_t cancellation_callback_count{0};
};

[[nodiscard]] voris::io::void_result terminal_result(race_terminal_kind kind) {
    using namespace voris::io;

    switch (kind) {
    case race_terminal_kind::cancelled:
    case race_terminal_kind::runtime_shutdown:
        return std::unexpected(make_error(vio_error_code::cancelled));
    case race_terminal_kind::closed:
        return std::unexpected(make_error(vio_error_code::closed));
    case race_terminal_kind::backend_success:
        return {};
    }

    return std::unexpected(make_error(vio_error_code::invalid_state));
}

void release_backend_reference_locked(race_operation_state& state) {
    if (state.submitted && !state.backend_reference_released) {
        state.backend_reference_released = true;
        ++state.backend_release_count;
    }
}

[[nodiscard]] bool select_terminal_locked(race_operation_state& state,
                                          race_terminal terminal) {
    if (state.terminal.has_value()) {
        return false;
    }

    state.terminal = std::move(terminal);
    if (state.observer_attached) {
        ++state.queued_count;
    }
    return true;
}

class race_operation_model {
public:
    explicit race_operation_model(voris::io::cancellation_token token)
        : state_(std::make_shared<race_operation_state>()) {
        std::weak_ptr<race_operation_state> weak_state = state_;
        registration_ = token.register_callback([weak_state, token](voris::io::cancellation_reason reason) {
            assert(!voris::io::detail::cancellation_internal_lock_held_for_testing(token));
            if (auto state = weak_state.lock()) {
                std::lock_guard guard(state->mutex);
                ++state->cancellation_callback_count;
                (void)select_terminal_locked(*state, race_terminal{
                                                         .kind = race_terminal_kind::cancelled,
                                                         .cancellation = reason,
                                                         .result = terminal_result(
                                                             race_terminal_kind::cancelled),
                                                     });
            }
        });
    }

    [[nodiscard]] race_handle_token token() const {
        std::lock_guard guard(state_->mutex);
        return state_->current;
    }

    [[nodiscard]] bool submit(race_handle_token token) {
        std::lock_guard guard(state_->mutex);
        if (state_->stopped || state_->terminal.has_value() || !state_->handle_open ||
            token != state_->current) {
            return false;
        }

        state_->submitted = true;
        return true;
    }

    [[nodiscard]] bool backend_succeed(race_handle_token token) {
        std::lock_guard guard(state_->mutex);
        if (!state_->submitted) {
            return false;
        }
        if (!state_->handle_open || token != state_->current) {
            ++state_->stale_event_count;
            return false;
        }

        release_backend_reference_locked(*state_);
        return select_terminal_locked(*state_, race_terminal{
                                                   .kind = race_terminal_kind::backend_success,
                                                   .cancellation = std::nullopt,
                                                   .result = terminal_result(
                                                       race_terminal_kind::backend_success),
                                               });
    }

    [[nodiscard]] bool close(race_handle_token token) {
        std::lock_guard guard(state_->mutex);
        if (!state_->handle_open || token != state_->current) {
            ++state_->stale_event_count;
            return false;
        }

        state_->handle_open = false;
        release_backend_reference_locked(*state_);
        return select_terminal_locked(*state_, race_terminal{
                                                   .kind = race_terminal_kind::closed,
                                                   .cancellation = std::nullopt,
                                                   .result = terminal_result(
                                                       race_terminal_kind::closed),
                                               });
    }

    [[nodiscard]] race_handle_token reopen_same_native_handle() {
        std::lock_guard guard(state_->mutex);
        assert(!state_->handle_open);
        ++state_->current.generation;
        state_->handle_open = true;
        return state_->current;
    }

    [[nodiscard]] bool shutdown() {
        std::lock_guard guard(state_->mutex);
        state_->stopped = true;
        state_->handle_open = false;
        release_backend_reference_locked(*state_);
        return select_terminal_locked(*state_,
                                      race_terminal{
                                          .kind = race_terminal_kind::runtime_shutdown,
                                          .cancellation =
                                              voris::io::cancellation_reason::runtime_shutdown,
                                          .result = terminal_result(
                                              race_terminal_kind::runtime_shutdown),
                                      });
    }

    [[nodiscard]] std::optional<race_terminal> terminal() const {
        std::lock_guard guard(state_->mutex);
        return state_->terminal;
    }

    [[nodiscard]] std::optional<race_terminal> observe_once() {
        std::lock_guard guard(state_->mutex);
        if (!state_->terminal.has_value() || !state_->observer_attached || state_->observed) {
            return std::nullopt;
        }

        state_->observed = true;
        ++state_->observed_count;
        return state_->terminal;
    }

    [[nodiscard]] std::size_t queued_count() const {
        std::lock_guard guard(state_->mutex);
        return state_->queued_count;
    }

    [[nodiscard]] std::size_t backend_release_count() const {
        std::lock_guard guard(state_->mutex);
        return state_->backend_release_count;
    }

    [[nodiscard]] std::size_t stale_event_count() const {
        std::lock_guard guard(state_->mutex);
        return state_->stale_event_count;
    }

    [[nodiscard]] std::size_t cancellation_callback_count() const {
        std::lock_guard guard(state_->mutex);
        return state_->cancellation_callback_count;
    }

private:
    std::shared_ptr<race_operation_state> state_;
    voris::io::cancellation_registration registration_;
};

void assert_terminal_is_valid_cancel_or_success(const race_terminal& terminal) {
    using namespace voris::io;

    if (terminal.kind == race_terminal_kind::cancelled) {
        assert(terminal.cancellation == cancellation_reason::manual);
        assert(!terminal.result.has_value());
        assert(terminal.result.error().classification == vio_error_code::cancelled);
        return;
    }

    assert(terminal.kind == race_terminal_kind::backend_success);
    assert(!terminal.cancellation.has_value());
    assert(terminal.result.has_value());
}

void assert_terminal_is_valid_close_or_success(const race_terminal& terminal) {
    using namespace voris::io;

    if (terminal.kind == race_terminal_kind::closed) {
        assert(!terminal.cancellation.has_value());
        assert(!terminal.result.has_value());
        assert(terminal.result.error().classification == vio_error_code::closed);
        return;
    }

    assert(terminal.kind == race_terminal_kind::backend_success);
    assert(terminal.result.has_value());
}

void assert_terminal_is_valid_shutdown_or_success(const race_terminal& terminal) {
    using namespace voris::io;

    if (terminal.kind == race_terminal_kind::runtime_shutdown) {
        assert(terminal.cancellation == cancellation_reason::runtime_shutdown);
        assert(!terminal.result.has_value());
        assert(terminal.result.error().classification == vio_error_code::cancelled);
        return;
    }

    assert(terminal.kind == race_terminal_kind::backend_success);
    assert(terminal.result.has_value());
}

[[nodiscard]] stress_counter stress_cancellation_race(const stress_config& config) {
    using namespace voris::io;

    return run_for_budget(config.iterations, config.duration, [](std::size_t) {
        cancellation_source source;
        race_operation_model operation(source.token());
        const auto token = operation.token();
        assert(operation.submit(token));

        std::barrier start(3);
        std::thread canceller([&] {
            start.arrive_and_wait();
            (void)source.request_cancellation(cancellation_reason::manual);
        });
        std::thread completer([&] {
            start.arrive_and_wait();
            (void)operation.backend_succeed(token);
        });

        start.arrive_and_wait();
        canceller.join();
        completer.join();

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_terminal_is_valid_cancel_or_success(*terminal);
        assert(operation.queued_count() == 1);
        assert(operation.backend_release_count() == 1);
        assert(operation.cancellation_callback_count() <= 1);
        assert(operation.observe_once().has_value());
        assert(!operation.observe_once().has_value());
    });
}

[[nodiscard]] stress_counter stress_close_race(const stress_config& config) {
    return run_for_budget(config.iterations, config.duration, [](std::size_t) {
        voris::io::cancellation_source source;
        race_operation_model operation(source.token());
        const auto token = operation.token();
        assert(operation.submit(token));

        std::barrier start(3);
        std::thread closer([&] {
            start.arrive_and_wait();
            (void)operation.close(token);
        });
        std::thread completer([&] {
            start.arrive_and_wait();
            (void)operation.backend_succeed(token);
        });

        start.arrive_and_wait();
        closer.join();
        completer.join();

        auto terminal = operation.terminal();
        assert(terminal.has_value());
        assert_terminal_is_valid_close_or_success(*terminal);
        assert(operation.queued_count() == 1);
        assert(operation.backend_release_count() == 1);

        if (terminal->kind == race_terminal_kind::closed) {
            const auto reused = operation.reopen_same_native_handle();
            assert(reused.native_handle == token.native_handle);
            assert(reused.generation == token.generation + 1);
            assert(!operation.backend_succeed(token));
            assert(operation.stale_event_count() >= 1);
        }
    });
}

[[nodiscard]] stress_counter stress_shutdown_race(const stress_config& config) {
    return run_for_budget(std::max<std::size_t>(1, config.iterations / 2),
                          config.duration,
                          [](std::size_t) {
                              voris::io::cancellation_source source;
                              race_operation_model operation(source.token());
                              const auto token = operation.token();
                              assert(operation.submit(token));

                              std::barrier start(3);
                              std::thread shutdown_thread([&] {
                                  start.arrive_and_wait();
                                  (void)operation.shutdown();
                              });
                              std::thread completer([&] {
                                  start.arrive_and_wait();
                                  (void)operation.backend_succeed(token);
                              });

                              start.arrive_and_wait();
                              shutdown_thread.join();
                              completer.join();

                              auto terminal = operation.terminal();
                              assert(terminal.has_value());
                              assert_terminal_is_valid_shutdown_or_success(*terminal);
                              assert(operation.queued_count() == 1);
                              assert(operation.backend_release_count() == 1);
                              assert(operation.observe_once().has_value());
                              assert(!operation.observe_once().has_value());
                          });
}

[[nodiscard]] voris::io::backend_operation backend_operation(
    std::size_t id,
    voris::io::backend_operation_kind kind,
    voris::io::backend_handle_token token) {
    return voris::io::backend_operation{
        .id = id,
        .kind = kind,
        .target = voris::io::backend_operation_target::socket,
        .scheduler = {},
        .handle = token,
    };
}

voris::io::backend_handle_token require_backend_token(
    voris::io::io_result<voris::io::backend_handle_token> result) {
    assert(result.has_value());
    return *result;
}

void assert_error_code(const voris::io::void_result& result,
                       voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

template<class T>
void assert_io_error_code(const voris::io::io_result<T>& result,
                          voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

void assert_completion_error(const voris::io::backend_completion& completion,
                             std::size_t operation_id,
                             voris::io::vio_error_code code) {
    assert(completion.operation_id == operation_id);
    assert_error_code(completion.result, code);
}

[[nodiscard]] stress_counter stress_virtual_backend_close_and_shutdown(
    const stress_config& config) {
    using namespace voris::io;

    return run_for_budget(config.iterations, config.duration, [](std::size_t iteration) {
        virtual_backend backend;
        const auto native_handle = 1000 + iteration;
        const auto first = require_backend_token(backend.register_handle(native_handle));
        assert(first.generation == 1);
        assert(backend.submit(backend_operation(10, backend_operation_kind::read, first))
                   .has_value());
        assert(backend.close_handle(first).has_value());
        assert(backend.poll().value() == 1);

        std::array<backend_completion, 4> completions{};
        auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        assert(*drained == 1);
        assert_completion_error(completions[0], 10, vio_error_code::closed);

        const auto reused = require_backend_token(backend.register_handle(native_handle));
        assert(reused.native_handle == first.native_handle);
        assert(reused.generation == first.generation + 1);
        assert_error_code(backend.close_handle(first), vio_error_code::invalid_state);
        assert(backend.poll().value() == 0);

        assert(backend.submit(backend_operation(11, backend_operation_kind::write, reused))
                   .has_value());
        assert(backend.cancel(11, cancellation_reason::manual).has_value());
        assert(backend.shutdown().has_value());
        assert(backend.stopped());
        assert_io_error_code(backend.register_handle(native_handle + 1), vio_error_code::closed);
        assert_error_code(backend.submit(backend_operation(12, backend_operation_kind::read,
                                                           reused)),
                          vio_error_code::closed);
        assert_error_code(backend.wake(), vio_error_code::closed);

        drained = backend.drain_completions(completions);
        assert(drained.has_value());
        assert(*drained == 1);
        assert_completion_error(completions[0], 11, vio_error_code::closed);
    });
}

[[nodiscard]] stress_counter stress_runtime_shutdown_submit_race(
    const stress_config& config) {
    using namespace voris::io;

    return run_for_budget(std::max<std::size_t>(1, config.iterations / 4),
                          config.duration,
                          [](std::size_t) {
                              runtime_options options;
                              options.shard_count = 2;
                              options.queue_limit = 128;
                              auto created = runtime::create(options);
                              assert(created.has_value());

                              std::atomic<std::size_t> accepted{0};
                              std::atomic<std::size_t> rejected{0};
                              std::atomic<std::size_t> completed{0};

                              created->start();

                              std::barrier start(3);
                              std::thread submitter([&] {
                                  start.arrive_and_wait();
                                  for (std::size_t i = 0; i != 64; ++i) {
                                      auto& shard = created->get_shard(i % created->shard_count());
                                      auto submitted = shard.submit([&completed] {
                                          completed.fetch_add(1);
                                      });
                                      if (submitted.has_value()) {
                                          accepted.fetch_add(1);
                                      } else {
                                          rejected.fetch_add(1);
                                      }
                                  }
                              });
                              std::thread stopper([&] {
                                  start.arrive_and_wait();
                                  created->request_stop();
                              });

                              start.arrive_and_wait();
                              submitter.join();
                              stopper.join();
                              created->join();

                              assert(accepted.load() + rejected.load() == 64);
                              assert(completed.load() == accepted.load());
                              for (std::size_t i = 0; i != created->shard_count(); ++i) {
                                  assert(!created->get_shard(i).running());
                                  assert(!created->get_shard(i).last_loop_error().has_value());
                              }
                          });
}

void run_vio_m8_001_cancellation_close_shutdown_stress() {
    const auto config = load_config();
    const auto cancellation = stress_cancellation_race(config);
    const auto close = stress_close_race(config);
    const auto shutdown = stress_shutdown_race(config);
    const auto backend = stress_virtual_backend_close_and_shutdown(config);
    const auto runtime = stress_runtime_shutdown_submit_race(config);

    std::printf("VIO_HARDENING_STRESS mode=%s iterations=%zu duration_seconds=%lld "
                "cancellation_iterations=%zu cancellation_ms=%lld "
                "close_iterations=%zu close_ms=%lld shutdown_iterations=%zu shutdown_ms=%lld "
                "backend_iterations=%zu backend_ms=%lld runtime_iterations=%zu runtime_ms=%lld\n",
                config.long_mode ? "long" : "quick",
                config.iterations,
                static_cast<long long>(config.duration.count()),
                cancellation.iterations,
                static_cast<long long>(cancellation.elapsed.count()),
                close.iterations,
                static_cast<long long>(close.elapsed.count()),
                shutdown.iterations,
                static_cast<long long>(shutdown.elapsed.count()),
                backend.iterations,
                static_cast<long long>(backend.elapsed.count()),
                runtime.iterations,
                static_cast<long long>(runtime.elapsed.count()));
}

} // namespace

int main() {
    run_vio_m8_001_cancellation_close_shutdown_stress();
    return 0;
}
