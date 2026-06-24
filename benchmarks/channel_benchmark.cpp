#include <voris/io/channel.hpp>

#include <iostream>

int main() {
    using namespace voris::io;

    channel<int> queue(1000);
    constexpr int count = 1000;
    for (int i = 0; i != count; ++i) {
        (void)queue.send(i);
    }
    int received = 0;
    while (queue.receive().has_value()) {
        ++received;
    }
    std::cout << "channel_received=" << received << '\n';
    return received == count ? 0 : 1;
}
