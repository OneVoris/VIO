#include <voris/io/shard.hpp>

#include "test_assert.hpp"
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

    shard saturated(1);
    int scheduled = 0;
    assert(saturated.submit([&scheduled] { scheduled += 1; }).has_value());
    auto rejected = saturated.scheduler().schedule([&scheduled] { scheduled += 10; });
    assert(!rejected.has_value());
    assert(rejected.error().classification == vio_error_code::resource_exhausted);
    assert(saturated.drain() == 1);
    assert(scheduled == 1);

    default_scheduler local;
    int default_ran = 0;
    assert(scheduler_ref(local).schedule([&default_ran] { ++default_ran; }).has_value());
    assert(local.run_one());
    assert(default_ran == 1);

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
