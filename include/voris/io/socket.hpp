#pragma once

#include <cstddef>
#include <deque>
#include <span>
#include <vector>

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

} // namespace voris::io
