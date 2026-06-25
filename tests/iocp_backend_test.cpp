#include <voris/io/backends/iocp_backend.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "test_assert.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = voris::io::backend_operation_kind::read;
    result.handle = token;
    return result;
}

voris::io::backend_operation file_operation(std::size_t id,
                                            voris::io::backend_operation_kind kind,
                                            voris::io::backend_handle_token token) {
    auto result = operation(id, token);
    result.kind = kind;
    result.target = voris::io::backend_operation_target::file;
    return result;
}

void assert_void_error(const voris::io::void_result& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_token_error(
    const voris::io::io_result<voris::io::backend_handle_token>& result,
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

voris::io::backends::detail::iocp_completion_key_token require_completion_key(
    voris::io::io_result<voris::io::backends::detail::iocp_completion_key_token> result) {
    assert(result.has_value());
    return *result;
}

void assert_closed_completion(const voris::io::backend_completion& completion,
                              std::size_t operation_id) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == voris::io::vio_error_code::closed);
}

void test_completion_key_round_trip_reserves_wake_sentinel() {
    using namespace voris::io;
    namespace iocp_detail = voris::io::backends::detail;

    const backend_handle_token large_opaque_token{
        std::numeric_limits<std::size_t>::max(),
        7U,
    };
    const iocp_detail::iocp_completion_key_token key_token{
        1U,
        large_opaque_token.generation,
    };
    const auto packed = iocp_detail::pack_iocp_completion_key(key_token);
    assert(packed.has_value());
    assert(*packed != iocp_detail::iocp_wake_completion_key);
    assert(!iocp_detail::is_iocp_wake_completion_key(*packed));

    const auto unpacked = iocp_detail::unpack_iocp_completion_key(*packed);
    assert(unpacked.has_value());
    assert(unpacked->association_id == key_token.association_id);
    assert(unpacked->generation == key_token.generation);
    assert(iocp_detail::is_iocp_wake_completion_key(iocp_detail::iocp_wake_completion_key));

    assert(!iocp_detail::unpack_iocp_completion_key(iocp_detail::iocp_wake_completion_key)
                .has_value());
    assert(!iocp_detail::pack_iocp_completion_key({0, 1}).has_value());
    assert(!iocp_detail::pack_iocp_completion_key({1, 0}).has_value());
    assert(!iocp_detail::pack_iocp_completion_key(
                {iocp_detail::iocp_completion_key_low_mask + 1U, 1U})
                .has_value());
    assert(!iocp_detail::pack_iocp_completion_key(
                {1U, iocp_detail::iocp_completion_key_low_mask + 1U})
                .has_value());
}

void test_batch_and_native_packet_limits_are_normalized_and_capped() {
    voris::io::backends::iocp_backend backend{
        voris::io::backends::iocp_backend_options{
            .completion_batch_limit = 0,
            .native_packet_capacity = 0,
            .association_capacity = 0,
        }};
    assert(backend.completion_batch_limit() == 1);
    assert(backend.native_packet_capacity() == 1);
    assert(backend.association_capacity() == 1);
    assert(backend.shutdown().has_value());

    voris::io::backends::iocp_backend huge_limits{
        voris::io::backends::iocp_backend_options{
            .completion_batch_limit = std::numeric_limits<std::size_t>::max(),
            .native_packet_capacity = std::numeric_limits<std::size_t>::max(),
            .association_capacity = std::numeric_limits<std::size_t>::max(),
        }};
    assert(huge_limits.completion_batch_limit() ==
           voris::io::backends::detail::iocp_max_completion_batch_limit);
    assert(huge_limits.native_packet_capacity() ==
           voris::io::backends::detail::iocp_max_native_packet_capacity);
    assert(huge_limits.association_capacity() ==
           voris::io::backends::detail::iocp_max_association_count);
    assert(huge_limits.shutdown().has_value());
}

#if defined(_WIN32)
class unique_handle {
public:
    explicit unique_handle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}

    ~unique_handle() {
        reset();
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept : handle_(other.release()) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            (void)::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

unique_handle make_overlapped_temp_file() {
    std::array<wchar_t, MAX_PATH> directory{};
    const DWORD directory_length =
        ::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    assert(directory_length > 0);
    assert(directory_length < directory.size());

    std::array<wchar_t, MAX_PATH> path{};
    assert(::GetTempFileNameW(directory.data(), L"vio", 0, path.data()) != 0);

    unique_handle file(::CreateFileW(
        path.data(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OVERLAPPED,
        nullptr));
    assert(file.get() != INVALID_HANDLE_VALUE);
    return file;
}

std::size_t native_handle_value(HANDLE handle) noexcept {
    return reinterpret_cast<std::size_t>(handle);
}

void assert_handle_is_still_open(HANDLE handle) {
    ::SetLastError(ERROR_SUCCESS);
    const DWORD type = ::GetFileType(handle);
    assert(type != FILE_TYPE_UNKNOWN || ::GetLastError() == ERROR_SUCCESS);
}

void test_wake_packets_are_polled_in_bounded_batches_without_user_completion() {
    voris::io::backends::iocp_backend backend{
        voris::io::backends::iocp_backend_options{
            .completion_batch_limit = 2,
            .native_packet_capacity = 2,
        }};

    for (int index = 0; index < 5; ++index) {
        assert(backend.wake().has_value());
    }

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);
    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);
    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    std::array<voris::io::backend_completion, 1> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
    assert(backend.shutdown().has_value());
}

void test_current_native_packet_backlog_is_bounded_and_drainable() {
    namespace iocp_detail = voris::io::backends::detail;

    auto file = make_overlapped_temp_file();
    voris::io::backends::iocp_backend backend{
        voris::io::backends::iocp_backend_options{
            .completion_batch_limit = 2,
            .native_packet_capacity = 1,
        }};
    const auto token = require_token(backend.register_handle(native_handle_value(file.get())));
    const auto key = require_completion_key(iocp_detail::iocp_completion_key_for(backend, token));

    assert(iocp_detail::post_iocp_test_packet(backend, key, 17U, nullptr).has_value());
    assert(iocp_detail::post_iocp_test_packet(backend, key, 19U, nullptr).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    assert(iocp_detail::iocp_native_packet_count(backend) == 1);

    auto over_capacity = backend.poll();
    assert(!over_capacity.has_value());
    assert(over_capacity.error().classification == voris::io::vio_error_code::resource_exhausted);
    assert(iocp_detail::iocp_native_packet_count(backend) == 1);

    std::array<iocp_detail::iocp_native_completion_packet, 1> native_packets{};
    auto drained_native = iocp_detail::drain_iocp_native_packets(backend, native_packets);
    assert(drained_native.has_value());
    assert(*drained_native == 1);
    assert(native_packets[0].bytes_transferred == 17U);
    assert(native_packets[0].handle == token);
    assert(native_packets[0].completion_key.association_id == key.association_id);
    assert(native_packets[0].completion_key.generation == key.generation);
    assert(iocp_detail::iocp_native_packet_count(backend) == 0);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    assert(iocp_detail::iocp_native_packet_count(backend) == 1);

    drained_native = iocp_detail::drain_iocp_native_packets(backend, native_packets);
    assert(drained_native.has_value());
    assert(*drained_native == 1);
    assert(native_packets[0].bytes_transferred == 19U);

    assert(backend.close_handle(token).has_value());
    assert(backend.shutdown().has_value());
}

void test_old_generation_native_packet_is_cleanup_only() {
    namespace iocp_detail = voris::io::backends::detail;

    auto file = make_overlapped_temp_file();
    voris::io::backends::iocp_backend backend;
    const auto token = require_token(backend.register_handle(native_handle_value(file.get())));
    const auto stale_key =
        require_completion_key(iocp_detail::iocp_completion_key_for(backend, token));

    assert(backend.close_handle(token).has_value());
    assert(iocp_detail::post_iocp_test_packet(backend, stale_key, 23U, nullptr).has_value());

    const auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    assert(iocp_detail::iocp_native_packet_count(backend) == 0);

    std::array<voris::io::backend_completion, 1> completions{};
    const auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    assert(backend.shutdown().has_value());
}

void test_register_associates_valid_handle_and_rejects_duplicate_and_stale_tokens() {
    auto file = make_overlapped_temp_file();
    voris::io::backends::iocp_backend backend;

    assert_token_error(backend.register_handle(0), voris::io::vio_error_code::invalid_state);

    const auto token = require_token(backend.register_handle(native_handle_value(file.get())));
    assert(token.native_handle == native_handle_value(file.get()));
    assert(token.generation == 1);

    assert_token_error(backend.register_handle(native_handle_value(file.get())),
                       voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.submit(operation(10, {})), voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.close_handle({}), voris::io::vio_error_code::invalid_state);

    assert(backend.submit(file_operation(11, voris::io::backend_operation_kind::read, token))
               .has_value());
    assert(backend.close_handle(token).has_value());
    assert_handle_is_still_open(file.get());

    assert_void_error(backend.submit(file_operation(12, voris::io::backend_operation_kind::write,
                                                    token)),
                      voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.close_handle(token), voris::io::vio_error_code::invalid_state);

    std::array<voris::io::backend_completion, 2> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_closed_completion(completions[0], 11);
    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    assert(backend.shutdown().has_value());
}

void test_association_failure_rolls_back_registry_state() {
    voris::io::backends::iocp_backend backend{
        voris::io::backends::iocp_backend_options{.association_capacity = 1}};
    constexpr std::size_t invalid_nonzero_handle = 1U;

    for (int attempt = 0; attempt < 4; ++attempt) {
        auto failed = backend.register_handle(invalid_nonzero_handle);
        assert(!failed.has_value());
        assert(failed.error().classification == voris::io::vio_error_code::backend_failure);
        assert(failed.error().provider_code.has_value());
    }

    assert(backend.association_capacity() == 1);

    assert(backend.shutdown().has_value());
}

void test_closed_association_reuses_capacity_with_stale_safe_generation() {
    namespace iocp_detail = voris::io::backends::detail;

    voris::io::backends::iocp_backend backend{
        voris::io::backends::iocp_backend_options{
            .completion_batch_limit = 2,
            .native_packet_capacity = 2,
            .association_capacity = 1,
        }};

    iocp_detail::iocp_completion_key_token previous_key{};
    bool has_previous_key = false;
    for (std::size_t iteration = 0; iteration < 4; ++iteration) {
        auto file = make_overlapped_temp_file();
        const auto token =
            require_token(backend.register_handle(native_handle_value(file.get())));
        const auto key =
            require_completion_key(iocp_detail::iocp_completion_key_for(backend, token));
        assert(key.association_id == 1U);

        if (has_previous_key) {
            assert(key.association_id == previous_key.association_id);
            assert(key.generation > previous_key.generation);

            assert(iocp_detail::post_iocp_test_packet(backend, previous_key, 29U, nullptr)
                       .has_value());
            auto polled = backend.poll();
            assert(polled.has_value());
            assert(*polled == 1);
            assert(iocp_detail::iocp_native_packet_count(backend) == 0);
        }

        assert(iocp_detail::post_iocp_test_packet(backend, key, 31U, nullptr).has_value());
        auto polled = backend.poll();
        assert(polled.has_value());
        assert(*polled == 1);
        assert(iocp_detail::iocp_native_packet_count(backend) == 1);

        std::array<iocp_detail::iocp_native_completion_packet, 1> native_packets{};
        auto drained_native = iocp_detail::drain_iocp_native_packets(backend, native_packets);
        assert(drained_native.has_value());
        assert(*drained_native == 1);
        assert(native_packets[0].handle == token);
        assert(native_packets[0].completion_key.association_id == key.association_id);
        assert(native_packets[0].completion_key.generation == key.generation);

        assert(backend.close_handle(token).has_value());
        previous_key = key;
        has_previous_key = true;
    }

    assert(backend.shutdown().has_value());
}

void test_close_completes_pending_work_once_and_does_not_close_caller_handle() {
    auto file = make_overlapped_temp_file();
    voris::io::backends::iocp_backend backend;
    const auto token = require_token(backend.register_handle(native_handle_value(file.get())));

    assert(backend.submit(file_operation(21, voris::io::backend_operation_kind::read, token))
               .has_value());
    assert(backend.submit(file_operation(22, voris::io::backend_operation_kind::write, token))
               .has_value());
    assert(backend.cancel(21, voris::io::cancellation_reason::manual).has_value());

    assert(backend.close_handle(token).has_value());
    assert_handle_is_still_open(file.get());

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 2);
    assert_closed_completion(completions[0], 21);
    assert_closed_completion(completions[1], 22);
    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
    assert_void_error(backend.cancel(21, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::invalid_state);

    assert(backend.shutdown().has_value());
}

void test_shutdown_is_idempotent_closes_port_and_drains_pending_work() {
    auto file = make_overlapped_temp_file();
    voris::io::backends::iocp_backend backend;
    const auto token = require_token(backend.register_handle(native_handle_value(file.get())));

    assert(backend.submit(file_operation(31, voris::io::backend_operation_kind::fsync, token))
               .has_value());
    assert(backend.shutdown().has_value());
    assert(backend.shutdown().has_value());

    assert_token_error(backend.register_handle(native_handle_value(file.get())),
                       voris::io::vio_error_code::closed);
    assert_void_error(backend.submit(file_operation(32, voris::io::backend_operation_kind::read,
                                                    token)),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.cancel(31, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.close_handle(token), voris::io::vio_error_code::closed);
    assert_void_error(backend.wake(), voris::io::vio_error_code::closed);
    assert_size_error(backend.poll(), voris::io::vio_error_code::closed);
    assert_handle_is_still_open(file.get());

    std::array<voris::io::backend_completion, 2> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_closed_completion(completions[0], 31);
}
#endif

} // namespace

int main() {
    using namespace voris::io;

    static_assert(!std::is_copy_constructible_v<backends::iocp_backend>);
    static_assert(!std::is_copy_assignable_v<backends::iocp_backend>);

    backends::overlapped_operation_lifetime lifetime{true, true, false};
    assert(lifetime.storage_retained());
    lifetime.completion_observed = true;
    assert(!lifetime.storage_retained());

    test_completion_key_round_trip_reserves_wake_sentinel();
    test_batch_and_native_packet_limits_are_normalized_and_capped();

    backends::iocp_backend backend;
#if defined(_WIN32)
    test_wake_packets_are_polled_in_bounded_batches_without_user_completion();
    test_current_native_packet_backlog_is_bounded_and_drainable();
    test_old_generation_native_packet_is_cleanup_only();
    test_register_associates_valid_handle_and_rejects_duplicate_and_stale_tokens();
    test_association_failure_rolls_back_registry_state();
    test_closed_association_reuses_capacity_with_stale_safe_generation();
    test_close_completes_pending_work_once_and_does_not_close_caller_handle();
    test_shutdown_is_idempotent_closes_port_and_drains_pending_work();

    auto file = make_overlapped_temp_file();
    auto token = backend.register_handle(native_handle_value(file.get()));
    assert(token.has_value());
    assert(backend.submit(file_operation(1, backend_operation_kind::read, *token)).has_value());
    assert(backend.poll().has_value());
    assert(backend.close_handle(*token).has_value());
    std::array<backend_completion, 2> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
#else
    auto result = backend.register_handle(1);
    assert(!result.has_value());
    assert(result.error().classification == vio_error_code::unsupported);
    assert_void_error(backend.submit(operation(1, {1, 1})), vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_size_error(backend.poll(), vio_error_code::unsupported);
    assert_void_error(backend.wake(), vio_error_code::unsupported);
    assert_void_error(backend.close_handle({1, 1}), vio_error_code::unsupported);
    std::array<backend_completion, 1> completions{};
    assert_size_error(backend.drain_completions(completions), vio_error_code::unsupported);
#endif
    assert(backend.shutdown().has_value());
    return 0;
}
