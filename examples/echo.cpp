#include <voris/io/socket.hpp>

#include <cassert>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> echo_messages(std::span<const std::string_view> input) {
    voris::io::socket_operation_queue operations(input.size() * 2);
    std::vector<std::string> echoed;
    echoed.reserve(input.size());

    std::size_t operation_id = 1;
    for (std::string_view message : input) {
        const std::size_t read_id = operation_id++;
        assert(operations.enqueue(voris::io::socket_operation_direction::read, read_id)
                   .has_value());
        auto ready_read = operations.pop(voris::io::socket_operation_direction::read);
        assert(ready_read.has_value());
        assert(*ready_read == read_id);

        const std::size_t write_id = operation_id++;
        assert(operations.enqueue(voris::io::socket_operation_direction::write, write_id)
                   .has_value());
        auto ready_write = operations.pop(voris::io::socket_operation_direction::write);
        assert(ready_write.has_value());
        assert(*ready_write == write_id);

        echoed.emplace_back(message);
    }

    assert(operations.size(voris::io::socket_operation_direction::read) == 0);
    assert(operations.size(voris::io::socket_operation_direction::write) == 0);
    return echoed;
}

} // namespace

int main() {
    const std::string_view payloads[] = {"vio", "echo", "done"};
    auto echoed = echo_messages(payloads);

    assert(echoed.size() == 3);
    assert(echoed[0] == "vio");
    assert(echoed[1] == "echo");
    assert(echoed[2] == "done");
    return 0;
}
