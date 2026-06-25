#include <voris/io/backends/io_uring_backend.hpp>

#include <array>
#include <cstddef>
#include <span>

#include "test_assert.hpp"

namespace voris::io::backends::detail {

[[nodiscard]] io_uring_capabilities capabilities_from_io_uring_probe_opcodes(
    std::span<const unsigned> supported_opcodes) noexcept;

} // namespace voris::io::backends::detail

namespace {

constexpr unsigned uapi_op_read = 22U;
constexpr unsigned uapi_op_write = 23U;
constexpr unsigned uapi_op_readv = 1U;
constexpr unsigned uapi_op_writev = 2U;
constexpr unsigned uapi_op_fsync = 3U;
constexpr unsigned uapi_op_read_fixed = 4U;
constexpr unsigned uapi_op_write_fixed = 5U;
constexpr unsigned uapi_op_accept = 13U;
constexpr unsigned uapi_op_async_cancel = 14U;
constexpr unsigned uapi_op_connect = 16U;
constexpr unsigned uapi_op_files_update = 20U;

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_operation_kind kind,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = kind;
    result.handle = token;
    return result;
}

void assert_void_unsupported(const voris::io::void_result& result) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::unsupported);
}

void assert_void_error(const voris::io::void_result& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

template <class T>
void assert_io_unsupported(const voris::io::io_result<T>& result) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::unsupported);
}

template <class T>
void assert_io_error(const voris::io::io_result<T>& result,
                     voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_completion_error(const voris::io::backend_completion& completion,
                             std::size_t operation_id,
                             voris::io::vio_error_code expected) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == expected);
}

voris::io::backends::io_uring_capabilities core_capabilities() {
    return voris::io::backends::io_uring_capabilities{
        .available = true,
        .supports_read = true,
        .supports_write = true,
        .supports_accept = true,
        .supports_connect = true,
        .supports_files = true,
        .supports_fsync = true,
        .supports_cancel = true,
    };
}

void assert_not_default_eligible_when(
    bool voris::io::backends::io_uring_capabilities::*field) {
    auto caps = core_capabilities();
    caps.*field = false;

    const voris::io::backends::io_uring_backend backend(caps);
    assert(!backend.default_eligible());
}

void assert_submit_unsupported_when(
    voris::io::backend_operation_kind kind,
    bool voris::io::backends::io_uring_capabilities::*field) {
    auto caps = core_capabilities();
    caps.*field = false;

    voris::io::backends::io_uring_backend backend(caps);
    auto token = backend.register_handle(1);
    assert(token.has_value());

    assert_void_unsupported(backend.submit(operation(10, kind, *token)));
    assert(backend.shutdown().has_value());
}

void assert_no_capabilities(const voris::io::backends::io_uring_capabilities& caps) {
    assert(!caps.available);
    assert(!caps.supports_read);
    assert(!caps.supports_write);
    assert(!caps.supports_accept);
    assert(!caps.supports_connect);
    assert(!caps.supports_files);
    assert(!caps.supports_fsync);
    assert(!caps.supports_cancel);
    assert(!caps.supports_registered_buffers);
    assert(!caps.supports_registered_files);
}

void test_default_eligibility_requires_core_capabilities() {
    const voris::io::backends::io_uring_backend eligible(core_capabilities());
    assert(eligible.default_eligible());

    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::available);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_read);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_write);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_accept);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_connect);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_files);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_fsync);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_cancel);
}

void test_probe_opcode_mapping_is_deterministic() {
    constexpr std::array supported{
        uapi_op_read,         uapi_op_write,     uapi_op_fsync,
        uapi_op_accept,       uapi_op_connect,   uapi_op_async_cancel,
        uapi_op_read_fixed,   uapi_op_write_fixed,
        uapi_op_files_update,
    };

    const auto caps =
        voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
            supported);

    assert(caps.available);
    assert(caps.supports_read);
    assert(caps.supports_write);
    assert(caps.supports_fsync);
    assert(caps.supports_files);
    assert(caps.supports_accept);
    assert(caps.supports_connect);
    assert(caps.supports_cancel);
    assert(caps.supports_registered_buffers);
    assert(caps.supports_registered_files);
}

void test_probe_files_require_read_write_and_fsync() {
    {
        constexpr std::array supported{uapi_op_read, uapi_op_write, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_fsync);
        assert(caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_write, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_read, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(!caps.supports_write);
        assert(caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_read, uapi_op_write};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(!caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_readv};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(!caps.supports_write);
        assert(!caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_writev};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_read);
        assert(caps.supports_write);
        assert(!caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_readv, uapi_op_writev, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_fsync);
        assert(caps.supports_files);
    }
}

void test_probe_registered_capabilities_are_independent_candidates() {
    {
        constexpr std::array supported{uapi_op_read_fixed};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_registered_buffers);
        assert(!caps.supports_registered_files);
    }

    {
        constexpr std::array supported{uapi_op_write_fixed};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_registered_buffers);
        assert(!caps.supports_registered_files);
    }

    {
        constexpr std::array supported{uapi_op_read_fixed, uapi_op_write_fixed};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_registered_buffers);
        assert(!caps.supports_registered_files);
    }

    {
        constexpr std::array supported{uapi_op_files_update};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_registered_buffers);
        assert(caps.supports_registered_files);
        assert(!caps.supports_files);
    }
}

void test_submit_rejects_missing_operation_opcodes() {
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::read,
        &voris::io::backends::io_uring_capabilities::supports_read);
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::write,
        &voris::io::backends::io_uring_capabilities::supports_write);
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::accept,
        &voris::io::backends::io_uring_capabilities::supports_accept);
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::connect,
        &voris::io::backends::io_uring_capabilities::supports_connect);
}

void test_optional_registrations_follow_capabilities() {
    {
        voris::io::backends::io_uring_backend backend(core_capabilities());
        assert_void_unsupported(backend.register_buffers(2));
        assert_void_unsupported(backend.register_files(2));
    }

    {
        auto caps = core_capabilities();
        caps.available = false;
        caps.supports_registered_buffers = true;
        caps.supports_registered_files = true;

        voris::io::backends::io_uring_backend backend(caps);
        assert_void_unsupported(backend.register_buffers(2));
        assert_void_unsupported(backend.register_files(2));
    }

    {
        auto caps = core_capabilities();
        caps.supports_registered_buffers = true;

        voris::io::backends::io_uring_backend backend(caps);
        assert(backend.register_buffers(2).has_value());
        assert_void_unsupported(backend.register_files(2));
    }

    {
        auto caps = core_capabilities();
        caps.supports_registered_files = true;

        voris::io::backends::io_uring_backend backend(caps);
        assert_void_unsupported(backend.register_buffers(2));
        assert(backend.register_files(2).has_value());
    }
}

void test_lifecycle_state_tracks_availability_and_shutdown() {
    {
        auto caps = core_capabilities();
        caps.available = false;

        voris::io::backends::io_uring_backend backend(caps);
        assert(backend.state() == voris::io::backends::io_uring_backend_state::unavailable);
        assert_io_unsupported(backend.register_handle(1));
        assert_io_unsupported(backend.poll());

        std::array<voris::io::backend_completion, 1> completions{};
        assert_io_unsupported(backend.drain_completions(completions));

        assert(backend.shutdown().has_value());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);
        assert_io_error(backend.register_handle(1), voris::io::vio_error_code::closed);
    }

    {
        voris::io::backends::io_uring_backend backend(core_capabilities());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::active);

        const auto token = backend.register_handle(1);
        assert(token.has_value());

        assert(backend.shutdown().has_value());
        assert(backend.shutdown().has_value());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);

        assert_io_error(backend.register_handle(2), voris::io::vio_error_code::closed);
        assert_void_error(backend.submit(operation(11, voris::io::backend_operation_kind::read,
                                                   *token)),
                          voris::io::vio_error_code::closed);
        assert_void_error(backend.wake(), voris::io::vio_error_code::closed);
        assert_io_error(backend.poll(), voris::io::vio_error_code::closed);
        assert_void_error(backend.register_buffers(1), voris::io::vio_error_code::closed);

        std::array<voris::io::backend_completion, 1> completions{};
        const auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        assert(*drained == 0);
    }
}

void test_empty_completion_drain_is_invalid_state() {
    auto caps = core_capabilities();
    caps.available = false;
    voris::io::backends::io_uring_backend unavailable(caps);
    assert_io_error(
        unavailable.drain_completions(std::span<voris::io::backend_completion>{}),
        voris::io::vio_error_code::invalid_state);

    voris::io::backends::io_uring_backend backend(core_capabilities());
    assert_io_error(backend.drain_completions(std::span<voris::io::backend_completion>{}),
                    voris::io::vio_error_code::invalid_state);
}

void test_submit_validates_operation_before_queueing() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), voris::io::backends::io_uring_backend_options{
                                 .submission_queue_capacity = 2,
                                 .submit_batch_limit = 1,
                                 .completion_batch_limit = 1,
                             });
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert_void_error(backend.submit(operation(0, voris::io::backend_operation_kind::read,
                                               *token)),
                      voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.submit(operation(1, voris::io::backend_operation_kind::read,
                                               {})),
                      voris::io::vio_error_code::invalid_state);

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert_void_error(backend.submit(operation(1, voris::io::backend_operation_kind::read,
                                               *token)),
                      voris::io::vio_error_code::invalid_state);
    assert(backend.shutdown().has_value());
}

void test_submission_queue_is_bounded() {
    {
        voris::io::backends::io_uring_backend backend(
            core_capabilities(), voris::io::backends::io_uring_backend_options{
                                     .submission_queue_capacity = 0,
                                     .submit_batch_limit = 2,
                                     .completion_batch_limit = 2,
                                 });
        const auto token = backend.register_handle(1);
        assert(token.has_value());
        assert_void_error(backend.submit(operation(1, voris::io::backend_operation_kind::read,
                                                   *token)),
                          voris::io::vio_error_code::resource_exhausted);
        assert(backend.shutdown().has_value());
    }

    {
        voris::io::backends::io_uring_backend backend(
            core_capabilities(), voris::io::backends::io_uring_backend_options{
                                     .submission_queue_capacity = 2,
                                     .submit_batch_limit = 2,
                                     .completion_batch_limit = 2,
                                 });
        const auto token = backend.register_handle(1);
        assert(token.has_value());

        assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
                   .has_value());
        assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
                   .has_value());
        assert_void_error(backend.submit(operation(3, voris::io::backend_operation_kind::read,
                                                   *token)),
                          voris::io::vio_error_code::resource_exhausted);
        assert(backend.shutdown().has_value());
    }
}

void test_queued_operation_can_be_cancelled_before_flush() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), voris::io::backends::io_uring_backend_options{
                                 .submission_queue_capacity = 4,
                                 .submit_batch_limit = 1,
                                 .completion_batch_limit = 4,
                             });
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
               .has_value());

    assert(backend.cancel(2, voris::io::cancellation_reason::manual).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.cancel(1, voris::io::cancellation_reason::manual).has_value());

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.close_handle(*token).has_value());
    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    std::array<voris::io::backend_completion, 2> completions{};
    const auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 2);
    assert_completion_error(completions[0], 1, voris::io::vio_error_code::closed);
    assert_completion_error(completions[1], 2, voris::io::vio_error_code::closed);
}

void test_poll_flushes_submissions_in_batches() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), voris::io::backends::io_uring_backend_options{
                                 .submission_queue_capacity = 8,
                                 .submit_batch_limit = 2,
                                 .completion_batch_limit = 8,
                             });
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(3, voris::io::backend_operation_kind::read, *token))
               .has_value());

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.cancel(1, voris::io::cancellation_reason::manual).has_value());
    assert(backend.cancel(2, voris::io::cancellation_reason::manual).has_value());
    assert(backend.cancel(3, voris::io::cancellation_reason::manual).has_value());

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.shutdown().has_value());
}

void test_poll_observes_completions_in_batches_and_drain_preserves_order() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), voris::io::backends::io_uring_backend_options{
                                 .submission_queue_capacity = 8,
                                 .submit_batch_limit = 8,
                                 .completion_batch_limit = 2,
                             });
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::write, *token))
               .has_value());
    assert(backend.submit(operation(3, voris::io::backend_operation_kind::read, *token))
               .has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.close_handle(*token).has_value());

    std::array<voris::io::backend_completion, 1> one_completion{};
    auto drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 0);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(one_completion[0], 1, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(one_completion[0], 2, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 0);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);

    std::array<voris::io::backend_completion, 4> completions{};
    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(completions[0], 3, voris::io::vio_error_code::closed);
}

void test_shutdown_closes_queued_and_pending_operations() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), voris::io::backends::io_uring_backend_options{
                                 .submission_queue_capacity = 8,
                                 .submit_batch_limit = 1,
                                 .completion_batch_limit = 8,
                             });
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(3, voris::io::backend_operation_kind::read, *token))
               .has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.shutdown().has_value());
    assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);
    assert_io_error(backend.poll(), voris::io::vio_error_code::closed);
    assert_void_error(backend.submit(operation(4, voris::io::backend_operation_kind::read,
                                               *token)),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.wake(), voris::io::vio_error_code::closed);

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 3);
    assert_completion_error(completions[0], 1, voris::io::vio_error_code::closed);
    assert_completion_error(completions[1], 2, voris::io::vio_error_code::closed);
    assert_completion_error(completions[2], 3, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
}

void test_detected_capabilities_are_conservative() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    voris::io::backends::io_uring_backend backend(caps);
    assert(backend.capabilities().available == caps.available);

#if defined(__linux__)
    if (!caps.available) {
        assert_no_capabilities(caps);
        assert(!backend.default_eligible());
        assert_io_unsupported(backend.register_handle(1));
        return;
    }

    if (backend.default_eligible()) {
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_accept);
        assert(caps.supports_connect);
        assert(caps.supports_files);
        assert(caps.supports_fsync);
        assert(caps.supports_cancel);
    }

    if (caps.supports_registered_buffers) {
        assert(backend.register_buffers(2).has_value());
    } else {
        assert_void_unsupported(backend.register_buffers(2));
    }

    if (caps.supports_registered_files) {
        assert(backend.register_files(2).has_value());
    } else {
        assert_void_unsupported(backend.register_files(2));
    }
#else
    assert_no_capabilities(caps);
    assert(!backend.default_eligible());
    assert_io_unsupported(backend.register_handle(1));
#endif
}

} // namespace

int main() {
    using namespace voris::io;

    test_default_eligibility_requires_core_capabilities();
    test_probe_opcode_mapping_is_deterministic();
    test_probe_files_require_read_write_and_fsync();
    test_probe_registered_capabilities_are_independent_candidates();
    test_submit_rejects_missing_operation_opcodes();
    test_optional_registrations_follow_capabilities();
    test_lifecycle_state_tracks_availability_and_shutdown();
    test_empty_completion_drain_is_invalid_state();
    test_submit_validates_operation_before_queueing();
    test_submission_queue_is_bounded();
    test_queued_operation_can_be_cancelled_before_flush();
    test_poll_flushes_submissions_in_batches();
    test_poll_observes_completions_in_batches_and_drain_preserves_order();
    test_shutdown_closes_queued_and_pending_operations();
    test_detected_capabilities_are_conservative();

    auto caps = backends::io_uring_capabilities{
        .available = true,
        .supports_read = true,
        .supports_write = true,
        .supports_accept = true,
        .supports_connect = true,
        .supports_files = true,
        .supports_fsync = true,
        .supports_cancel = true,
    };
    backends::io_uring_backend backend(caps);
    auto token = backend.register_handle(1);
    assert(token.has_value());
    assert(backend.submit(operation(1, backend_operation_kind::read, *token)).has_value());
    assert(backend.poll().has_value());
    assert(backend.cancel(1, cancellation_reason::manual).has_value());
    assert(backend.close_handle(*token).has_value());
    assert(backend.poll().has_value());
    std::array<backend_completion, 2> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert(backend.shutdown().has_value());

    return 0;
}
