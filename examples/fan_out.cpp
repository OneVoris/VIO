#include <voris/io/when_all.hpp>

#include <cassert>
#include <tuple>

namespace {

voris::io::task<int> worker(int input) {
    co_return input * 10;
}

} // namespace

int main() {
    using namespace voris::io;

    voris::io::default_scheduler scheduler;
    voris::io::scheduler_ref ref(scheduler);
    voris::io::current_scheduler_scope scope(ref);

    auto fan_out = voris::io::when_all(worker(1), worker(2));
    auto result = std::move(fan_out).take_result();
    assert(result.has_value());

    auto& branches = *result;
    assert(std::get<0>(branches).has_value());
    assert(std::get<1>(branches).has_value());

    const int total = *std::get<0>(branches) + *std::get<1>(branches);
    assert(total == 30);
    return 0;
}
