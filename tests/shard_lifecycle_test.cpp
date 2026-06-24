#include <voris/io/shard.hpp>
#include <voris/io/trampoline.hpp>

#include "test_assert.hpp"
#include <atomic>
#include <vector>

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

    shard direct_saturated(1);
    int direct_scheduled = 0;
    assert(direct_saturated.submit([&direct_scheduled] { direct_scheduled += 1; }).has_value());
    auto direct_rejected = direct_saturated.enqueue([&direct_scheduled] { direct_scheduled += 10; });
    assert(!direct_rejected.has_value());
    assert(direct_rejected.error().classification == vio_error_code::resource_exhausted);
    assert(direct_saturated.drain() == 1);
    assert(direct_scheduled == 1);

    shard trampoline_saturated(1);
    int trampoline_scheduled = 0;
    assert(trampoline_saturated.submit([&trampoline_scheduled] { trampoline_scheduled += 1; })
               .has_value());
    auto trampoline_rejected =
        trampoline::schedule(trampoline_saturated.scheduler(),
                             [&trampoline_scheduled] { trampoline_scheduled += 10; });
    assert(!trampoline_rejected.has_value());
    assert(trampoline_rejected.error().classification == vio_error_code::resource_exhausted);
    assert(trampoline_saturated.drain() == 1);
    assert(trampoline_scheduled == 1);

    shard system_saturated(1);
    std::vector<int> system_order;
    assert(system_saturated.submit([&system_order] { system_order.push_back(2); }).has_value());
    assert(trampoline::schedule_system(system_saturated.scheduler(),
                                       [&system_order] { system_order.push_back(1); })
               .has_value());
    assert(system_saturated.drain() == 2);
    assert(system_order == std::vector<int>({1, 2}));

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
