#include <voris/io/shard.hpp>

#include <cassert>
#include <atomic>

int main() {
    using namespace voris::io;

    shard owner(4);
    int ran = 0;
    assert(owner.submit([&ran] { ++ran; }).has_value());
    assert(owner.drain() == 1);
    assert(ran == 1);
    assert(owner.metrics().submitted_tasks == 1);
    assert(owner.metrics().completed_tasks == 1);

    owner.start();
    assert(owner.running());
    std::atomic<int> async_ran{0};
    assert(owner.submit([&async_ran] { ++async_ran; }).has_value());
    while (async_ran.load() == 0) {
        std::this_thread::yield();
    }
    owner.request_stop();
    owner.join();
    assert(!owner.running());
    assert(owner.thread_id() != std::thread::id{});

    return 0;
}
