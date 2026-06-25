#include <voris/io/runtime.hpp>
#include <voris/io/submit.hpp>

#include "test_assert.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void assert_error(const voris::io::void_result& result, voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

void join_with_timeout(voris::io::shard& current) {
    using namespace std::chrono_literals;

    std::mutex mutex;
    std::condition_variable joined;
    bool join_returned = false;

    std::thread joiner([&] {
        current.join();
        {
            std::lock_guard lock(mutex);
            join_returned = true;
        }
        joined.notify_one();
    });

    {
        std::unique_lock lock(mutex);
        assert(joined.wait_for(lock, 2s, [&] { return join_returned; }));
    }

    joiner.join();
}

void join_runtime_with_timeout(voris::io::runtime& current) {
    for (std::size_t index = 0; index != current.shard_count(); ++index) {
        join_with_timeout(current.get_shard(index));
    }
}

} // namespace

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    {
        runtime_options options;
        options.shard_count = 2;
        options.queue_limit = 2;
        auto created = runtime::create(options);
        assert(created.has_value());

        auto& first = created->get_shard(0);
        auto& second = created->get_shard(1);

        int value = 0;
        assert(submit_to(first, [&] {
            value += 1;
            assert(submit_to(second, [&] { value += 2; }).has_value());
        }).has_value());

        assert(first.drain() == 1);
        assert(second.drain() == 1);
        assert(value == 3);
    }

    {
        runtime_options options;
        options.shard_count = 1;
        options.queue_limit = 2;
        auto created = runtime::create(options);
        assert(created.has_value());

        auto& first = created->get_shard(0);

        continuation empty_user;
        assert_error(first.submit(std::move(empty_user)), vio_error_code::invalid_state);

        continuation empty_system;
        assert_error(first.submit_system(std::move(empty_system)), vio_error_code::invalid_state);

        assert(first.drain() == 0);
    }

    {
        runtime_options options;
        options.shard_count = 2;
        options.queue_limit = 1;
        options.scheduler_budget =
            loop_budget{.task_budget = 8, .completion_budget = 8, .timer_budget = 8};
        auto created = runtime::create(options);
        assert(created.has_value());

        auto& first = created->get_shard(0);

        std::vector<int> order;
        assert(first.submit([&order] { order.push_back(2); }).has_value());

        bool rejected_user_ran = false;
        auto rejected_user = first.submit([&rejected_user_ran] { rejected_user_ran = true; });
        assert_error(rejected_user, vio_error_code::resource_exhausted);

        assert(first.submit_system([&order] { order.push_back(1); }).has_value());

        bool rejected_system_ran = false;
        auto rejected_system =
            first.submit_system([&rejected_system_ran] { rejected_system_ran = true; });
        assert_error(rejected_system, vio_error_code::resource_exhausted);

        auto turn = first.run_one_loop_iteration();
        assert(turn.has_value());
        assert(*turn == 2);
        assert(order == std::vector<int>({1, 2}));
        assert(!rejected_user_ran);
        assert(!rejected_system_ran);
    }

    {
        runtime_options options;
        options.shard_count = 1;
        options.queue_limit = 2;
        auto created = runtime::create(options);
        assert(created.has_value());

        auto& first = created->get_shard(0);
        created->start();
        assert(first.wait_until_parked_for(2s));

        first.request_stop();
        join_with_timeout(first);

        assert(!first.running());
        assert(first.thread_id() != std::thread::id{});

        bool stopped_user_ran = false;
        auto stopped_user = first.submit([&stopped_user_ran] { stopped_user_ran = true; });
        assert_error(stopped_user, vio_error_code::invalid_state);

        bool stopped_system_ran = false;
        auto stopped_system =
            first.submit_system([&stopped_system_ran] { stopped_system_ran = true; });
        assert_error(stopped_system, vio_error_code::invalid_state);

        assert(!stopped_user_ran);
        assert(!stopped_system_ran);
    }

    {
        runtime_options options;
        options.shard_count = 2;
        options.queue_limit = 4;
        auto created = runtime::create(options);
        assert(created.has_value());

        auto& owner = created->get_shard(0);
        auto& target = created->get_shard(1);
        const auto owner_scheduler = owner.scheduler();
        const auto target_scheduler = target.scheduler();

        std::mutex mutex;
        std::condition_variable returned;
        std::vector<int> order;
        int result = 0;
        bool target_scheduler_restored = false;
        bool owner_scheduler_restored = false;
        bool return_done = false;

        created->start();

        assert(submit_to(target_scheduler,
                         [owner_scheduler, target_scheduler, &mutex, &returned, &order,
                          &result, &target_scheduler_restored, &owner_scheduler_restored,
                          &return_done] {
                             auto current_target = require_current_scheduler();
                             {
                                 std::lock_guard lock(mutex);
                                 target_scheduler_restored =
                                     current_target.has_value() &&
                                     *current_target == target_scheduler;
                                 order.push_back(1);
                             }

                             auto payload = std::make_unique<int>(41);
                             auto submitted =
                                 submit_to(owner_scheduler,
                                           [owned = std::move(payload), owner_scheduler, &mutex,
                                            &returned, &order, &result,
                                            &owner_scheduler_restored, &return_done] {
                                               auto current_owner = require_current_scheduler();
                                               {
                                                   std::lock_guard lock(mutex);
                                                   owner_scheduler_restored =
                                                       current_owner.has_value() &&
                                                       *current_owner == owner_scheduler;
                                                   result = *owned + 1;
                                                   order.push_back(2);
                                                   return_done = true;
                                               }
                                               returned.notify_one();
                                           });
                             assert(payload == nullptr);
                             assert(submitted.has_value());
                         })
                   .has_value());

        {
            std::unique_lock lock(mutex);
            assert(returned.wait_for(lock, 2s, [&] { return return_done; }));
            assert(order == std::vector<int>({1, 2}));
            assert(result == 42);
            assert(target_scheduler_restored);
            assert(owner_scheduler_restored);
        }

        assert(!current_scheduler().has_value());
        created->request_stop();
        join_runtime_with_timeout(*created);
    }

    {
        std::atomic<bool> return_ran{false};

        {
            runtime_options options;
            options.shard_count = 2;
            options.queue_limit = 4;
            auto created = runtime::create(options);
            assert(created.has_value());

            auto& owner = created->get_shard(0);
            auto& target = created->get_shard(1);
            const auto owner_scheduler = owner.scheduler();
            const auto target_scheduler = target.scheduler();

            std::mutex mutex;
            std::condition_variable attempted;
            bool return_attempted = false;

            created->start();
            assert(owner.wait_until_parked_for(2s));

            owner.request_stop();
            join_with_timeout(owner);
            assert(!owner.running());

            assert(submit_to(target_scheduler,
                             [owner_scheduler, &mutex, &attempted, &return_attempted,
                              &return_ran] {
                                 auto submitted = submit_to(owner_scheduler, [&return_ran] {
                                     return_ran.store(true);
                                 });
                                 assert_error(submitted, vio_error_code::invalid_state);
                                 {
                                     std::lock_guard lock(mutex);
                                     return_attempted = true;
                                 }
                                 attempted.notify_one();
                             })
                       .has_value());

            {
                std::unique_lock lock(mutex);
                assert(attempted.wait_for(lock, 2s, [&] { return return_attempted; }));
            }

            assert(!return_ran.load());

            target.request_stop();
            join_with_timeout(target);
            assert(!target.running());
        }

        assert(!return_ran.load());
    }

    return 0;
}
