#include <voris/io/submit.hpp>

#include <cassert>
#include <memory>

int main() {
    using namespace voris::io;

    shard target(2);
    auto value = std::make_unique<int>(7);
    int observed = 0;

    assert(submit_to(target, [owned = std::move(value), &observed] {
        observed = *owned;
    }).has_value());
    assert(value == nullptr);
    assert(target.drain() == 1);
    assert(observed == 7);

    return 0;
}
