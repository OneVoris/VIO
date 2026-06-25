#include <voris/io/channel.hpp>
#include <voris/io/cancellation.hpp>
#include <voris/io/deadline.hpp>

#include "test_assert.hpp"
#include <chrono>

template<class Result>
void assert_error(const Result& result, voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

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

    {
        channel<int> full(1);
        assert(full.send(1).has_value());

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        assert_error(full.send(2, source.token()), vio_error_code::cancelled);
        assert(full.waiting_senders() == 0);
    }

    {
        channel<int> rendezvous_send(0);

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        assert_error(rendezvous_send.send(1, source.token()), vio_error_code::cancelled);
        assert(rendezvous_send.waiting_senders() == 0);
    }

    {
        channel<int> empty(0);

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        assert_error(empty.receive(source.token()), vio_error_code::cancelled);
        assert(empty.waiting_receivers() == 0);
    }

    {
        channel<int> full(1);
        assert(full.send(1).has_value());

        const deadline expired{deadline::time_point{10ms}};
        const auto now = deadline::time_point{10ms};

        assert_error(full.send(2, expired, now), vio_error_code::deadline_exceeded);
        assert(full.waiting_senders() == 0);
    }

    {
        channel<int> empty(0);

        const deadline expired{deadline::time_point{10ms}};
        const auto now = deadline::time_point{10ms};

        assert_error(empty.receive(expired, now), vio_error_code::deadline_exceeded);
        assert(empty.waiting_receivers() == 0);
    }

    {
        channel<int> full(1);
        assert(full.send(1).has_value());

        const deadline future{deadline::time_point{11ms}};
        const auto now = deadline::time_point{10ms};

        assert_error(full.send(2, future, now), vio_error_code::resource_exhausted);
        assert(full.waiting_senders() == 1);
    }

    {
        channel<int> empty(0);

        const deadline future{deadline::time_point{11ms}};
        const auto now = deadline::time_point{10ms};

        assert_error(empty.receive(future, now), vio_error_code::resource_exhausted);
        assert(empty.waiting_receivers() == 1);
    }

    {
        channel<int> ready(1);
        assert(ready.send(42).has_value());

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        auto value = ready.receive(source.token());
        assert(value.has_value());
        assert(*value == 42);
        assert(ready.waiting_receivers() == 0);
    }

    {
        channel<int> ready(1);
        assert(ready.send(43).has_value());

        const deadline expired{deadline::time_point{10ms}};
        const auto now = deadline::time_point{10ms};

        auto value = ready.receive(expired, now);
        assert(value.has_value());
        assert(*value == 43);
        assert(ready.waiting_receivers() == 0);
    }

    {
        channel<int> closed_channel(1);
        closed_channel.close();

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        const deadline expired{deadline::time_point{10ms}};
        const auto now = deadline::time_point{10ms};

        assert_error(closed_channel.send(1, source.token()), vio_error_code::closed);
        assert_error(closed_channel.send(1, expired, now), vio_error_code::closed);
        assert_error(closed_channel.receive(source.token()), vio_error_code::closed);
        assert_error(closed_channel.receive(expired, now), vio_error_code::closed);
    }

    return 0;
}
