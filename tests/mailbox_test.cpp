#include <voris/io/detail/mailbox.hpp>

#include "test_assert.hpp"
#include <vector>

int main() {
    using namespace voris::io;

    detail::mailbox mailbox(2);
    std::vector<int> order;
    assert(mailbox.submit([&order] { order.push_back(1); }).has_value());
    assert(mailbox.submit([&order] { order.push_back(2); }).has_value());
    assert(!mailbox.submit([&order] { order.push_back(3); }).has_value());

    assert(mailbox.run_one());
    assert(order == std::vector<int>{1});
    assert(mailbox.submit([&order] { order.push_back(3); }).has_value());
    assert(mailbox.run_until_idle() == 2);
    assert(order == std::vector<int>({1, 2, 3}));

    return 0;
}
