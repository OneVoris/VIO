#include <voris/io/when_all.hpp>

#include "test_assert.hpp"

namespace {

voris::io::task<int> value_task(int value) {
    co_return value;
}

voris::io::task<void> void_task() {
    co_return;
}

} // namespace

int main() {
    using namespace voris::io;

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scheduler_scope(ref);

    auto combined = when_all(value_task(1), void_task(), value_task(3));
    auto result = std::move(combined).take_result();
    assert(result.has_value());

    auto& tuple = *result;
    assert(std::get<0>(tuple).has_value());
    assert(*std::get<0>(tuple) == 1);
    assert(std::get<1>(tuple).has_value());
    assert(std::get<2>(tuple).has_value());
    assert(*std::get<2>(tuple) == 3);

    return 0;
}
