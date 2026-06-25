#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

struct iocp_backend_options {
    std::size_t completion_batch_limit{32};
};

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

struct iocp_native_completion_packet {
    std::size_t bytes_transferred{};
    std::uintptr_t completion_key{};
    void* overlapped{};
    std::uintptr_t internal_status{};
};

[[nodiscard]] inline bool is_iocp_wake_completion_key(std::uintptr_t key) noexcept {
    return key == iocp_wake_completion_key;
}

[[nodiscard]] inline io_result<std::uintptr_t> pack_iocp_completion_key(
    backend_handle_token token) {
    if (token.native_handle == 0 || token.generation == 0 ||
        token.native_handle > iocp_completion_key_low_mask ||
        token.generation > iocp_completion_key_low_mask) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP completion key cannot encode handle token"));
    }

    const auto key =
        (static_cast<std::uintptr_t>(token.generation)
         << iocp_completion_key_generation_shift) |
        static_cast<std::uintptr_t>(token.native_handle);
    if (is_iocp_wake_completion_key(key)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP completion key conflicts with wake sentinel"));
    }
    return key;
}

[[nodiscard]] inline io_result<backend_handle_token> unpack_iocp_completion_key(
    std::uintptr_t key) {
    if (is_iocp_wake_completion_key(key)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP wake sentinel is not a handle token"));
    }

    backend_handle_token token{
        static_cast<std::size_t>(key & iocp_completion_key_low_mask),
        static_cast<std::size_t>(key >> iocp_completion_key_generation_shift),
    };
    if (token.native_handle == 0 || token.generation == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP completion key is not a handle token"));
    }
    return token;
}

} // namespace detail

class iocp_backend final : public backend {
public:
    explicit iocp_backend(iocp_backend_options options = {});
    ~iocp_backend() override;

    iocp_backend(const iocp_backend&) = delete;
    iocp_backend& operator=(const iocp_backend&) = delete;

    [[nodiscard]] std::size_t completion_batch_limit() const noexcept;

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
#if defined(_WIN32)
    void close_owned_port() noexcept;
    [[nodiscard]] void_result initialization_result() const;
    [[nodiscard]] io_result<std::size_t> observe_native_completions();
#endif

    iocp_backend_options options_;
    virtual_backend fallback_;
    std::deque<detail::iocp_native_completion_packet> native_packets_{};
    void* completion_port_{};
    bool stopped_{false};
    std::optional<vio_error> initialization_error_{};
};

} // namespace voris::io::backends
