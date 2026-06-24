#include <voris/io/detail/bounded_queue.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    detail::bounded_queue<int> queue(2);
    assert(queue.try_push(1).has_value());
    assert(queue.try_push(2).has_value());
    auto full = queue.try_push(3);
    assert(!full.has_value());
    assert(full.error().classification == vio_error_code::resource_exhausted);
    assert(queue.capacity_waiters() == 1);

    auto first = queue.pop();
    assert(first.has_value());
    assert(*first == 1);
    assert(queue.capacity_waiters() == 0);

    assert(queue.try_push(3).has_value());
    assert(*queue.pop() == 2);
    assert(*queue.pop() == 3);
    assert(!queue.pop().has_value());

    return 0;
}
