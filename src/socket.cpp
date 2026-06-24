#include <voris/io/socket.hpp>

namespace voris::io {

void_result socket_operation_queue::enqueue(socket_operation_direction direction,
                                            std::size_t operation_id) {
    auto& queue = direction == socket_operation_direction::read ? reads_ : writes_;
    if (queue.size() >= limit_) {
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "socket operation queue is full"));
    }
    queue.push_back(operation_id);
    return {};
}

std::optional<std::size_t> socket_operation_queue::pop(socket_operation_direction direction) {
    auto& queue = direction == socket_operation_direction::read ? reads_ : writes_;
    if (queue.empty()) {
        return std::nullopt;
    }
    const auto value = queue.front();
    queue.pop_front();
    return value;
}

std::size_t socket_operation_queue::size(socket_operation_direction direction) const noexcept {
    const auto& queue = direction == socket_operation_direction::read ? reads_ : writes_;
    return queue.size();
}

std::size_t total_size(std::span<const buffer_chain_view> buffers) noexcept {
    std::size_t total = 0;
    for (const auto& buffer : buffers) {
        total += buffer.bytes.size();
    }
    return total;
}

} // namespace voris::io
