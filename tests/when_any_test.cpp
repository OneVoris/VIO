#include <voris/io/when_any.hpp>

#include "test_assert.hpp"

namespace {

voris::io::task<int> value_task(int value) {
    co_return value;
}

} // namespace

int main() {
    using namespace voris::io;

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scheduler_scope(ref);

    cancellation_source losers;
    auto any = when_any(losers, value_task(10), value_task(20));
    auto result = std::move(any).take_result();
    assert(result.has_value());
    assert(result->index == 0);
    assert(std::get<0>(result->result).has_value());
    assert(*std::get<0>(result->result) == 10);
    assert(losers.cancellation_requested());
    assert(losers.reason() == cancellation_reason::manual);

    return 0;
}
