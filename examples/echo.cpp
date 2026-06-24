#include <voris/io/socket.hpp>

#include <cassert>

int main() {
    voris::io::socket_operation_queue queue(4);
    assert(queue.enqueue(voris::io::socket_operation_direction::read, 1).has_value());
    assert(queue.enqueue(voris::io::socket_operation_direction::write, 2).has_value());
    return 0;
}
