#include <voris/io/runtime.hpp>

#include "test_assert.hpp"

#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(!std::is_constructible_v<voris::io::runtime, voris::io::runtime_options>);

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    {
        runtime_options invalid;
        invalid.shard_count = 0;
        auto invalid_runtime = runtime::create(invalid);
        assert(!invalid_runtime.has_value());
        assert(invalid_runtime.error().classification == vio_error_code::invalid_state);
    }

    {
        runtime_options invalid;
        invalid.queue_limit = 0;
        auto invalid_runtime = runtime::create(invalid);
        assert(!invalid_runtime.has_value());
        assert(invalid_runtime.error().classification == vio_error_code::invalid_state);
    }

    {
        runtime_options invalid;
        invalid.scheduler_budget.task_budget = 0;
        auto invalid_runtime = runtime::create(invalid);
        assert(!invalid_runtime.has_value());
        assert(invalid_runtime.error().classification == vio_error_code::invalid_state);
    }

    {
        runtime_options options;
        options.shard_count = 2;
        options.queue_limit = 1;
        auto created = runtime::create(options);
        assert(created.has_value());
        assert(created->shard_count() == 2);
        assert(created->options().queue_limit == 1);

        auto& first = created->get_shard(0);
        assert(first.submit([] {}).has_value());
        auto rejected = first.submit([] {});
        assert(!rejected.has_value());
        assert(rejected.error().classification == vio_error_code::resource_exhausted);
    }

    {
        runtime_options options;
        options.scheduler_budget =
            loop_budget{.task_budget = 1, .completion_budget = 8, .timer_budget = 8};
        auto created = runtime::create(options);
        assert(created.has_value());

        std::vector<int> order;
        auto& first = created->get_shard(0);
        assert(first.submit([&order] { order.push_back(1); }).has_value());
        assert(first.submit([&order] { order.push_back(2); }).has_value());

        auto first_turn = first.run_one_loop_iteration();
        assert(first_turn.has_value());
        assert(*first_turn == 1);
        assert(order == std::vector<int>({1}));

        auto second_turn = first.run_one_loop_iteration();
        assert(second_turn.has_value());
        assert(*second_turn == 1);
        assert(order == std::vector<int>({1, 2}));
    }

    {
        runtime_options options;
        options.metrics_config.long_task_threshold = 5ms;
        auto created = runtime::create(options);
        assert(created.has_value());

        auto& first = created->get_shard(0);
        assert(first.submit([] { std::this_thread::sleep_for(20ms); }).has_value());
        assert(first.drain() == 1);
        assert(first.metrics().long_tasks == 1);
    }

    {
        runtime_options options;
        options.shard_count = 3;
        options.cpu_affinity_start = 3;
        auto created = runtime::create(options);
        assert(created.has_value());
        assert(created->requested_shard_cpu_affinity(0) == std::optional<std::size_t>(3));
        assert(created->requested_shard_cpu_affinity(1) == std::optional<std::size_t>(4));
        assert(created->requested_shard_cpu_affinity(2) == std::optional<std::size_t>(5));
    }

    {
        runtime_options invalid;
        invalid.shard_count = 2;
        invalid.cpu_affinity_start = std::numeric_limits<std::size_t>::max();
        auto invalid_runtime = runtime::create(invalid);
        assert(!invalid_runtime.has_value());
        assert(invalid_runtime.error().classification == vio_error_code::invalid_state);
    }

    {
        runtime_options options;
        options.shard_count = 1;
        options.cpu_affinity_start = std::numeric_limits<std::size_t>::max();
        auto created = runtime::create(options);
        assert(created.has_value());
        assert(created->requested_shard_cpu_affinity(0) ==
               std::optional<std::size_t>(std::numeric_limits<std::size_t>::max()));
    }

    {
        runtime_options options;
        auto created = runtime::create(options);
        assert(created.has_value());
        assert(!created->requested_shard_cpu_affinity(0).has_value());
    }

    return 0;
}
