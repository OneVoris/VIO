#include <voris/io/shard.hpp>

#include <iostream>

int main() {
    using namespace voris::io;

    shard current(1024);
    constexpr int count = 1000;
    int ran = 0;
    for (int i = 0; i != count; ++i) {
        (void)current.submit([&ran] { ++ran; });
    }
    const auto drained = current.drain();
    std::cout << "scheduler_submitted=" << count << '\n';
    std::cout << "scheduler_drained=" << drained << '\n';
    return ran == count ? 0 : 1;
}
