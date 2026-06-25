#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <span>

#include <voris/io/error.hpp>

namespace voris::io {

enum class socket_operation_direction {
    read,
    write,
};

class socket_operation_queue {
public:
    explicit socket_operation_queue(std::size_t limit)
        : limit_(limit) {}

    [[nodiscard]] void_result enqueue(socket_operation_direction direction, std::size_t operation_id);
    [[nodiscard]] std::optional<std::size_t> pop(socket_operation_direction direction);
    [[nodiscard]] std::size_t size(socket_operation_direction direction) const noexcept;

private:
    std::size_t limit_;
    std::deque<std::size_t> reads_;
    std::deque<std::size_t> writes_;
};

struct buffer_chain_view {
    std::span<const std::byte> bytes;
};

[[nodiscard]] std::size_t total_size(std::span<const buffer_chain_view> buffers) noexcept;

/// Borrowed native socket helpers. The caller keeps ownership and must pass an
/// already-nonblocking socket descriptor or handle. Non-empty I/O is currently
/// implemented on Linux; `write_some` is socket-only because Linux uses
/// `send(..., MSG_NOSIGNAL)`, so ordinary pipe or file descriptors may fail
/// with provider `ENOTSOCK`.
[[nodiscard]] io_result<std::size_t> read_some(std::size_t native_handle,
                                               std::span<std::byte> buffer);
[[nodiscard]] io_result<std::size_t> write_some(std::size_t native_handle,
                                                std::span<const std::byte> buffer);

} // namespace voris::io
