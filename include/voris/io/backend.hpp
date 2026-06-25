#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_set>
#include <vector>

#include <voris/io/cancellation.hpp>
#include <voris/io/detail/native_handle_registry.hpp>
#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

namespace backends {
class epoll_backend;
class io_uring_backend;
class iocp_backend;
class kqueue_backend;
} // namespace backends

enum class backend_operation_kind {
    read,
    write,
    accept,
    connect,
    fsync,
    close,
    wake,
};

enum class backend_operation_target {
    socket,
    file,
};

struct backend_handle_token {
    std::size_t native_handle{};
    std::size_t generation{};

    [[nodiscard]] friend bool operator==(backend_handle_token lhs,
                                         backend_handle_token rhs) noexcept = default;
};

struct backend_operation {
    std::size_t id{};
    backend_operation_kind kind{};
    backend_operation_target target{backend_operation_target::socket};
    scheduler_ref scheduler{};
    backend_handle_token handle{};
    // Borrowed payload storage must remain alive until the operation reaches
    // terminal completion and that completion is drained or detached according
    // to the backend contract.
    std::span<std::byte> read_buffer{};
    std::span<const std::byte> write_buffer{};
    std::span<const std::byte> socket_address{};
    std::uint64_t offset{};
};

struct backend_completion {
    std::size_t operation_id{};
    void_result result{};
    std::size_t bytes_transferred{};
    std::size_t accepted_native_handle{};
};

[[nodiscard]] bool is_socket_backend_operation(const backend_operation& operation) noexcept;
[[nodiscard]] bool is_file_backend_operation(const backend_operation& operation) noexcept;
[[nodiscard]] bool is_valid_backend_operation_shape(const backend_operation& operation) noexcept;

class backend {
public:
    virtual ~backend() = default;

    [[nodiscard]] virtual io_result<backend_handle_token> register_handle(
        std::size_t native_handle) = 0;
    [[nodiscard]] virtual void_result submit(backend_operation operation) = 0;
    [[nodiscard]] virtual void_result cancel(std::size_t operation_id,
                                             cancellation_reason reason) = 0;
    [[nodiscard]] virtual void_result close_handle(backend_handle_token token) = 0;
    [[nodiscard]] virtual io_result<std::size_t> poll() = 0;
    [[nodiscard]] virtual io_result<std::size_t> drain_completions(
        std::span<backend_completion> out) = 0;
    [[nodiscard]] virtual void_result wake() = 0;
    [[nodiscard]] virtual void_result shutdown() = 0;
};

class virtual_backend final : public backend {
public:
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

    [[nodiscard]] std::size_t submitted() const noexcept;
    [[nodiscard]] std::size_t cancelled() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;

private:
    friend class backends::epoll_backend;
    friend class backends::io_uring_backend;
    friend class backends::iocp_backend;
    friend class backends::kqueue_backend;

    struct pending_operation {
        backend_operation operation{};
    };

    [[nodiscard]] bool is_current_handle(backend_handle_token token) const noexcept;
    [[nodiscard]] void_result complete_ready(backend_handle_token token,
                                             backend_operation_kind readiness_kind);
    void complete_pending(backend_handle_token token, const void_result& result);
    void complete_pending(backend_handle_token token,
                          backend_operation_kind readiness_kind,
                          const void_result& result);

    detail::native_handle_registry registry_{};
    std::vector<pending_operation> pending_{};
    std::deque<backend_completion> completions_{};
    std::unordered_set<std::size_t> active_operation_ids_{};
    std::size_t registered_{0};
    std::size_t submitted_{0};
    std::size_t cancelled_{0};
    std::size_t wakeups_{0};
    bool stopped_{false};
};

} // namespace voris::io
