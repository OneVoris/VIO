#include <voris/io/when_all.hpp>

#include <cassert>

namespace {

voris::io::task<int> value(int v) {
    co_return v;
}

} // namespace

int main() {
    voris::io::default_scheduler scheduler;
    voris::io::scheduler_ref ref(scheduler);
    voris::io::current_scheduler_scope scope(ref);
    auto result = std::move(voris::io::when_all(value(1), value(2))).take_result();
    assert(result.has_value());
    return 0;
}
