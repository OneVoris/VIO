#include <voris/io/async_scope.hpp>

#include <cassert>
#include <stdexcept>

namespace {

voris::io::task<int> ok_task() {
    co_return 7;
}

voris::io::task<int> failing_task() {
    throw std::runtime_error("scope failure");
    co_return 0;
}

voris::io::task<void> ok_void_task() {
    co_return;
}

} // namespace

int main() {
    using namespace voris::io;

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scheduler_scope(ref);

    background_error_sink sink;
    async_scope scope(&sink);

    assert(scope.pending_count() == 0);
    assert(scope.spawn(ok_task()).has_value());
    assert(scope.spawn(ok_void_task()).has_value());
    assert(scope.join().has_value());
    assert(scope.errors().empty());
    assert(sink.empty());

    auto failure = scope.spawn(failing_task());
    assert(!failure.has_value());
    assert(failure.error().classification == vio_error_code::invalid_state);
    assert(scope.errors().size() == 1);
    assert(sink.size() == 1);
    assert(!scope.join().has_value());

    assert(scope.request_stop());
    assert(scope.stop_requested());
    assert(scope.token().reason() == cancellation_reason::scope_shutdown);
    assert(!scope.request_stop(cancellation_reason::runtime_shutdown));

    auto rejected = scope.spawn(ok_task());
    assert(!rejected.has_value());
    assert(rejected.error().classification == vio_error_code::cancelled);

    return 0;
}
