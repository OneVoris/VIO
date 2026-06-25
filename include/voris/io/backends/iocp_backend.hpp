#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

class iocp_backend;

struct overlapped_operation_lifetime {
    bool submitted{};
    bool cancellation_requested{};
    bool completion_observed{};

    [[nodiscard]] bool storage_retained() const noexcept {
        return submitted && !completion_observed;
    }
};

namespace detail {

inline constexpr unsigned iocp_completion_key_bits = sizeof(std::uintptr_t) * CHAR_BIT;
inline constexpr unsigned iocp_completion_key_generation_shift =
    iocp_completion_key_bits / 2U;
inline constexpr std::uintptr_t iocp_completion_key_low_mask =
    (std::uintptr_t{1} << iocp_completion_key_generation_shift) - 1U;
inline constexpr std::uintptr_t iocp_wake_completion_key =
    std::numeric_limits<std::uintptr_t>::max();
inline constexpr std::size_t iocp_default_completion_batch_limit = 32U;
inline constexpr std::size_t iocp_max_completion_batch_limit = 256U;
inline constexpr std::size_t iocp_max_native_packet_capacity = 1024U;
inline constexpr std::size_t iocp_max_association_count =
    iocp_completion_key_low_mask < 32768U ? iocp_completion_key_low_mask : 32768U;
inline constexpr std::uintptr_t iocp_status_success = 0U;
inline constexpr std::uintptr_t iocp_status_operation_aborted = 995U;
inline constexpr std::uintptr_t iocp_status_cancelled = 0xC0000120U;

struct iocp_completion_key_token {
    std::size_t association_id{};
    std::size_t generation{};
};

struct iocp_native_completion_packet {
    std::size_t bytes_transferred{};
    iocp_completion_key_token completion_key{};
    std::uintptr_t raw_completion_key{};
    backend_handle_token handle{};
    void* overlapped{};
    std::uintptr_t internal_status{};
};

[[nodiscard]] inline bool is_iocp_wake_completion_key(std::uintptr_t key) noexcept {
    return key == iocp_wake_completion_key;
}

[[nodiscard]] inline io_result<std::uintptr_t> pack_iocp_completion_key(
    iocp_completion_key_token token) {
    if (token.association_id == 0 || token.generation == 0 ||
        token.association_id > iocp_completion_key_low_mask ||
        token.generation > iocp_completion_key_low_mask) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP completion key cannot encode association token"));
    }

    const auto key =
        (static_cast<std::uintptr_t>(token.generation)
         << iocp_completion_key_generation_shift) |
        static_cast<std::uintptr_t>(token.association_id);
    if (is_iocp_wake_completion_key(key)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP completion key conflicts with wake sentinel"));
    }
    return key;
}

[[nodiscard]] inline io_result<iocp_completion_key_token> unpack_iocp_completion_key(
    std::uintptr_t key) {
    if (is_iocp_wake_completion_key(key)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP wake sentinel is not an association token"));
    }

    iocp_completion_key_token token{
        static_cast<std::size_t>(key & iocp_completion_key_low_mask),
        static_cast<std::size_t>(key >> iocp_completion_key_generation_shift),
    };
    if (token.association_id == 0 || token.generation == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP completion key is not an association token"));
    }
    return token;
}

[[nodiscard]] io_result<iocp_completion_key_token> iocp_completion_key_for(
    const iocp_backend& backend,
    backend_handle_token token);
[[nodiscard]] void_result post_iocp_test_packet(iocp_backend& backend,
                                                iocp_completion_key_token key,
                                                std::size_t bytes_transferred,
                                                void* overlapped);
[[nodiscard]] void_result queue_iocp_test_packet(iocp_backend& backend,
                                                 iocp_completion_key_token key,
                                                 std::size_t bytes_transferred,
                                                 void* overlapped,
                                                 std::uintptr_t internal_status);
[[nodiscard]] io_result<void*> iocp_overlapped_for(iocp_backend& backend,
                                                   std::size_t operation_id);
[[nodiscard]] void_result mark_iocp_operation_native_submitted(
    iocp_backend& backend,
    std::size_t operation_id);
[[nodiscard]] std::size_t iocp_operation_storage_count(
    const iocp_backend& backend) noexcept;
[[nodiscard]] std::size_t iocp_active_operation_id_count(
    const iocp_backend& backend) noexcept;
[[nodiscard]] std::size_t iocp_cancel_request_count(
    const iocp_backend& backend) noexcept;
[[nodiscard]] std::size_t iocp_native_packet_count(const iocp_backend& backend) noexcept;
[[nodiscard]] io_result<std::size_t> drain_iocp_native_packets(
    iocp_backend& backend,
    std::span<iocp_native_completion_packet> out);

} // namespace detail

struct iocp_backend_options {
    std::size_t completion_batch_limit{detail::iocp_default_completion_batch_limit};
    // Zero means "match the normalized completion batch limit".
    std::size_t native_packet_capacity{};
    // Zero is normalized to one reusable association slot.
    std::size_t association_capacity{detail::iocp_max_association_count};
};

class iocp_backend final : public backend {
public:
    explicit iocp_backend(iocp_backend_options options = {});
    ~iocp_backend() override;

    iocp_backend(const iocp_backend&) = delete;
    iocp_backend& operator=(const iocp_backend&) = delete;

    [[nodiscard]] std::size_t completion_batch_limit() const noexcept;
    [[nodiscard]] std::size_t native_packet_capacity() const noexcept;
    [[nodiscard]] std::size_t association_capacity() const noexcept;

    [[nodiscard]] io_result<backend_handle_token> register_handle(
        std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] void_result close_handle(backend_handle_token token) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] io_result<std::size_t> drain_completions(
        std::span<backend_completion> out) override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

private:
    friend io_result<detail::iocp_completion_key_token> detail::iocp_completion_key_for(
        const iocp_backend& backend,
        backend_handle_token token);
    friend void_result detail::post_iocp_test_packet(iocp_backend& backend,
                                                     detail::iocp_completion_key_token key,
                                                     std::size_t bytes_transferred,
                                                     void* overlapped);
    friend void_result detail::queue_iocp_test_packet(
        iocp_backend& backend,
        detail::iocp_completion_key_token key,
        std::size_t bytes_transferred,
        void* overlapped,
        std::uintptr_t internal_status);
    friend io_result<void*> detail::iocp_overlapped_for(iocp_backend& backend,
                                                        std::size_t operation_id);
    friend void_result detail::mark_iocp_operation_native_submitted(
        iocp_backend& backend,
        std::size_t operation_id);
    friend std::size_t detail::iocp_operation_storage_count(
        const iocp_backend& backend) noexcept;
    friend std::size_t detail::iocp_active_operation_id_count(
        const iocp_backend& backend) noexcept;
    friend std::size_t detail::iocp_cancel_request_count(
        const iocp_backend& backend) noexcept;
    friend std::size_t detail::iocp_native_packet_count(
        const iocp_backend& backend) noexcept;
    friend io_result<std::size_t> detail::drain_iocp_native_packets(
        iocp_backend& backend,
        std::span<detail::iocp_native_completion_packet> out);

    struct association_entry {
        std::size_t id{};
        backend_handle_token token{};
        std::size_t generation{};
        bool open{};
        bool reusable{};
        bool bump_generation_on_reuse{};
    };
    struct operation_storage;

    [[nodiscard]] io_result<detail::iocp_completion_key_token> create_association(
        backend_handle_token token);
    void rollback_association(detail::iocp_completion_key_token key) noexcept;
    void close_association(backend_handle_token token) noexcept;
    [[nodiscard]] io_result<detail::iocp_completion_key_token> completion_key_for(
        backend_handle_token token) const;
    [[nodiscard]] std::optional<backend_handle_token> current_handle_for(
        detail::iocp_completion_key_token key) const noexcept;
    [[nodiscard]] void_result validate_operation_for_submit(
        const backend_operation& operation) const;
    [[nodiscard]] void_result request_cancel_for(operation_storage& storage);
    [[nodiscard]] void_result mark_close_requested(backend_handle_token token);
    [[nodiscard]] void_result mark_shutdown_requested();
    [[nodiscard]] std::size_t observe_queued_native_packets();
    void observe_native_packet(const detail::iocp_native_completion_packet& packet);
    void complete_operation_storage(operation_storage& storage, void_result result);
    void complete_native_operation(operation_storage& storage,
                                   std::size_t bytes_transferred,
                                   std::uintptr_t internal_status);
    void erase_operation_storage(std::size_t operation_id);
    void maybe_close_stopped_port() noexcept;

#if defined(_WIN32)
    void close_owned_port() noexcept;
    [[nodiscard]] void_result initialization_result() const;
    [[nodiscard]] io_result<std::size_t> observe_native_completions();
#endif

    iocp_backend_options options_;
    virtual_backend fallback_;
    std::deque<detail::iocp_native_completion_packet> native_packets_{};
    std::deque<backend_completion> completion_queue_{};
    std::unordered_map<std::size_t, std::unique_ptr<operation_storage>> operations_{};
    std::list<std::size_t> operation_submission_order_{};
    std::unordered_map<std::size_t, std::list<std::size_t>::iterator> operation_order_by_id_{};
    std::unordered_map<void*, std::size_t> operation_id_by_overlapped_{};
    std::unordered_set<std::size_t> active_operation_ids_{};
    std::vector<association_entry> associations_{};
    std::vector<std::size_t> free_association_ids_{};
    std::unordered_map<std::size_t, std::size_t> association_by_native_handle_{};
    void* completion_port_{};
    bool stopped_{false};
    std::optional<vio_error> initialization_error_{};
    std::size_t cancel_request_count_{};
};

} // namespace voris::io::backends
