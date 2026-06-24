#include <voris/io/socket.hpp>

#include <array>
#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    socket_operation_queue queue(2);
    assert(queue.enqueue(socket_operation_direction::read, 1).has_value());
    assert(queue.enqueue(socket_operation_direction::read, 2).has_value());
    assert(!queue.enqueue(socket_operation_direction::read, 3).has_value());
    assert(queue.enqueue(socket_operation_direction::write, 4).has_value());
    assert(*queue.pop(socket_operation_direction::read) == 1);
    assert(*queue.pop(socket_operation_direction::read) == 2);
    assert(!queue.pop(socket_operation_direction::read).has_value());
    assert(*queue.pop(socket_operation_direction::write) == 4);

    std::array<std::byte, 3> first{};
    std::array<std::byte, 5> second{};
    std::array<buffer_chain_view, 2> buffers{{
        buffer_chain_view{std::span<const std::byte>(first)},
        buffer_chain_view{std::span<const std::byte>(second)},
    }};
    assert(total_size(buffers) == 8);

    return 0;
}
