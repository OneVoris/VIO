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

struct move_counted {
    static inline int move_count = 0;

    int value{0};

    explicit move_counted(int stored_value)
        : value(stored_value) {}

    move_counted(const move_counted&) = delete;
    move_counted& operator=(const move_counted&) = delete;

    move_counted(move_counted&& other) noexcept
        : value(other.value) {
        ++move_count;
        other.value = -1;
    }

    move_counted& operator=(move_counted&& other) noexcept {
        ++move_count;
        value = other.value;
        other.value = -1;
        return *this;
    }
};

void reset_move_count() {
    move_counted::move_count = 0;
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
        channel<int> full(1);
        assert(full.send(1).has_value());

        const auto now = deadline::time_point{10ms};

        assert_error(full.send(2, deadline::none(), now), vio_error_code::resource_exhausted);
        assert(full.waiting_senders() == 1);
    }

    {
        channel<int> empty(0);

        const auto now = deadline::time_point{10ms};

        assert_error(empty.receive(deadline::none(), now), vio_error_code::resource_exhausted);
        assert(empty.waiting_receivers() == 1);
    }

    {
        channel<int> ready_send_with_cancelled_token(1);

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        assert(ready_send_with_cancelled_token.send(44, source.token()).has_value());
        assert(ready_send_with_cancelled_token.size() == 1);
        assert(*ready_send_with_cancelled_token.receive() == 44);
    }

    {
        channel<int> ready_send_with_expired_deadline(1);

        const deadline expired{deadline::time_point{10ms}};
        const auto now = deadline::time_point{10ms};

        assert(ready_send_with_expired_deadline.send(45, expired, now).has_value());
        assert(ready_send_with_expired_deadline.size() == 1);
        assert(*ready_send_with_expired_deadline.receive() == 45);
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

    {
        channel<int> empty(0);

        cancellation_source source;

        assert_error(empty.receive(source.token()), vio_error_code::resource_exhausted);
        assert(empty.waiting_receivers() == 1);

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(empty.waiting_receivers() == 1);

        assert(empty.send(46).has_value());
        assert(*empty.receive() == 46);
    }

    {
        channel<move_counted> full(1);
        move_counted initial{1};
        assert(full.send(std::move(initial)).has_value());

        reset_move_count();
        move_counted failed{2};

        assert_error(full.send(std::move(failed)), vio_error_code::resource_exhausted);
        assert(move_counted::move_count == 1);
    }

    {
        channel<move_counted> full(1);
        move_counted initial{1};
        assert(full.send(std::move(initial)).has_value());

        cancellation_source source;
        assert(source.request_cancellation(cancellation_reason::manual));

        reset_move_count();
        move_counted cancelled{2};

        assert_error(full.send(std::move(cancelled), source.token()), vio_error_code::cancelled);
        assert(move_counted::move_count == 1);
    }

    {
        channel<move_counted> full(1);
        move_counted initial{1};
        assert(full.send(std::move(initial)).has_value());

        const deadline expired{deadline::time_point{10ms}};
        const auto now = deadline::time_point{10ms};

        reset_move_count();
        move_counted timed_out{2};

        assert_error(full.send(std::move(timed_out), expired, now),
                     vio_error_code::deadline_exceeded);
        assert(move_counted::move_count == 1);
    }

    {
        channel<move_counted> closed_channel(1);
        closed_channel.close();

        reset_move_count();
        move_counted closed_value{1};

        assert_error(closed_channel.send(std::move(closed_value)), vio_error_code::closed);
        assert(move_counted::move_count == 1);
    }

    return 0;
}
