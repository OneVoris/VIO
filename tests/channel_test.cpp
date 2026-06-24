#include <voris/io/channel.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    channel<int> buffered(2);
    assert(buffered.send(1).has_value());
    assert(buffered.send(2).has_value());
    assert(!buffered.send(3).has_value());
    assert(buffered.waiting_senders() == 1);
    assert(*buffered.receive() == 1);
    assert(buffered.waiting_senders() == 0);
    buffered.close();
    assert(*buffered.receive() == 2);
    assert(!buffered.receive().has_value());

    channel<int> rendezvous(0);
    auto empty_receive = rendezvous.receive();
    assert(!empty_receive.has_value());
    assert(rendezvous.waiting_receivers() == 1);
    assert(rendezvous.send(7).has_value());
    assert(*rendezvous.receive() == 7);

    return 0;
}
