#include <voris/io/runtime.hpp>

#include <atomic>
#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;

    voris::io::runtime_options options;
    options.shard_count = 1;
    options.queue_limit = 4;

    auto runtime = voris::io::runtime::create(options);
    assert(runtime.has_value());

    std::atomic<int> accepted_work{0};
    runtime->start();

    auto& shard = runtime->get_shard(0);
    assert(shard.wait_until_parked_for(2s));

    assert(shard.submit([&accepted_work] { accepted_work.fetch_add(1); }).has_value());
    assert(shard.submit([&accepted_work] { accepted_work.fetch_add(1); }).has_value());

    runtime->request_stop();
    runtime->join();

    assert(accepted_work.load() == 2);

    bool rejected_work_ran = false;
    auto post_stop = shard.submit([&rejected_work_ran] { rejected_work_ran = true; });
    assert(!post_stop.has_value());
    assert(post_stop.error().classification == voris::io::vio_error_code::invalid_state);
    assert(!rejected_work_ran);
    return 0;
}
