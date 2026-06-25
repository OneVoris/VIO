#include <voris/io/backend.hpp>

#include <array>
#include <span>

#include "test_assert.hpp"

namespace {

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_operation_kind kind,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = kind;
    result.handle = token;
    return result;
}

void assert_void_error(const voris::io::void_result& result, voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_token_error(const voris::io::io_result<voris::io::backend_handle_token>& result,
                        voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_size_error(const voris::io::io_result<std::size_t>& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

voris::io::backend_handle_token require_token(
    voris::io::io_result<voris::io::backend_handle_token> result) {
    assert(result.has_value());
    return *result;
}

std::size_t drain(voris::io::backend& backend,
                  std::span<voris::io::backend_completion> completions) {
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    return *drained;
}

void assert_completion(const voris::io::backend_completion& completion,
                       std::size_t operation_id,
                       voris::io::vio_error_code expected) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == expected);
}

void test_register_submit_close_and_drain_contract() {
    using namespace voris::io;

    virtual_backend backend;
    auto first = require_token(backend.register_handle(1));
    auto second = require_token(backend.register_handle(2));
    assert(first.native_handle == 1);
    assert(first.generation == 1);
    assert(second.native_handle == 2);
    assert(second.generation == 1);
    assert_token_error(backend.register_handle(0), vio_error_code::invalid_state);

    assert(backend.submit(operation(11, backend_operation_kind::read, first)).has_value());
    assert(backend.submit(operation(12, backend_operation_kind::write, first)).has_value());
    assert(backend.submit(operation(21, backend_operation_kind::read, second)).has_value());
    assert(backend.submitted() == 3);

    assert_void_error(backend.submit(operation(0, backend_operation_kind::read, first)),
                      vio_error_code::invalid_state);
    assert_void_error(backend.submit(operation(13, backend_operation_kind::read, {})),
                      vio_error_code::invalid_state);

    assert(backend.close_handle(first).has_value());
    assert_void_error(backend.submit(operation(14, backend_operation_kind::read, first)),
                      vio_error_code::invalid_state);

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    std::array<backend_completion, 8> completions{};
    assert(drain(backend, completions) == 2);
    assert_completion(completions[0], 11, vio_error_code::closed);
    assert_completion(completions[1], 12, vio_error_code::closed);
    assert(drain(backend, completions) == 0);

    assert_void_error(backend.cancel(11, cancellation_reason::manual), vio_error_code::invalid_state);
    assert(drain(backend, completions) == 0);

    auto still_pending = backend.poll();
    assert(still_pending.has_value());
    assert(*still_pending == 0);

    assert(backend.close_handle(second).has_value());
    assert(drain(backend, completions) == 1);
    assert_completion(completions[0], 21, vio_error_code::closed);
    assert_void_error(backend.close_handle(second), vio_error_code::invalid_state);
}

void test_reuse_requires_current_generation() {
    using namespace voris::io;

    virtual_backend backend;
    auto first = require_token(backend.register_handle(33));
    assert(backend.close_handle(first).has_value());

    auto reused = require_token(backend.register_handle(33));
    assert(reused.native_handle == first.native_handle);
    assert(reused.generation > first.generation);

    assert_void_error(backend.submit(operation(1, backend_operation_kind::read, first)),
                      vio_error_code::invalid_state);
    assert_void_error(backend.close_handle(first), vio_error_code::invalid_state);
    assert(backend.submit(operation(2, backend_operation_kind::read, reused)).has_value());
    assert(backend.close_handle(reused).has_value());
}

void test_operation_id_cannot_reuse_until_completion_is_drained() {
    using namespace voris::io;

    virtual_backend backend;
    auto closing = require_token(backend.register_handle(50));
    auto other = require_token(backend.register_handle(51));

    assert(backend.submit(operation(77, backend_operation_kind::read, closing)).has_value());
    assert(backend.close_handle(closing).has_value());

    assert_void_error(backend.submit(operation(77, backend_operation_kind::write, other)),
                      vio_error_code::invalid_state);

    std::array<backend_completion, 2> completions{};
    assert(drain(backend, completions) == 1);
    assert_completion(completions[0], 77, vio_error_code::closed);

    assert(backend.submit(operation(77, backend_operation_kind::write, other)).has_value());
    assert(backend.close_handle(other).has_value());
    assert(drain(backend, completions) == 1);
    assert_completion(completions[0], 77, vio_error_code::closed);
}

void test_shutdown_rejects_new_work_and_drains_pending_as_closed() {
    using namespace voris::io;

    virtual_backend backend;
    auto token = require_token(backend.register_handle(44));
    assert(backend.submit(operation(1, backend_operation_kind::read, token)).has_value());

    assert(backend.shutdown().has_value());
    assert(backend.stopped());
    assert_void_error(backend.submit(operation(2, backend_operation_kind::write, token)),
                      vio_error_code::closed);
    assert_void_error(backend.cancel(1, cancellation_reason::manual), vio_error_code::closed);
    assert_size_error(backend.drain_completions(std::span<backend_completion>{}),
                      vio_error_code::invalid_state);

    std::array<backend_completion, 2> completions{};
    assert(drain(backend, completions) == 1);
    assert_completion(completions[0], 1, vio_error_code::closed);
}

} // namespace

int main() {
    test_register_submit_close_and_drain_contract();
    test_reuse_requires_current_generation();
    test_operation_id_cannot_reuse_until_completion_is_drained();
    test_shutdown_rejects_new_work_and_drains_pending_as_closed();

    return 0;
}
