#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

#include <voris/io/cancellation.hpp>
#include <voris/io/deadline.hpp>
#include <voris/io/error.hpp>

namespace voris::io {

class channel_base {
public:
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

protected:
    bool closed_{false};
};

template<class T>
class channel : public channel_base {
public:
    explicit channel(std::size_t capacity)
        : capacity_(capacity) {}

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return buffer_.size();
    }

    [[nodiscard]] void_result send(T value) {
        return send_impl(value, [] { return std::optional<vio_error>{}; });
    }

    [[nodiscard]] void_result send(T value, const cancellation_token& token) {
        return send_impl(value, [&token] { return cancellation_wait_error(token); });
    }

    template<class Clock, class Duration>
        requires(Clock::is_steady)
    [[nodiscard]] void_result send(T value,
                                   const deadline& limit,
                                   std::chrono::time_point<Clock, Duration> now) {
        return send_impl(value, [&limit, now] { return deadline_wait_error(limit, now); });
    }

    [[nodiscard]] io_result<T> receive() {
        return receive_impl([] { return std::optional<vio_error>{}; });
    }

    [[nodiscard]] io_result<T> receive(const cancellation_token& token) {
        return receive_impl([&token] { return cancellation_wait_error(token); });
    }

    template<class Clock, class Duration>
        requires(Clock::is_steady)
    [[nodiscard]] io_result<T> receive(const deadline& limit,
                                       std::chrono::time_point<Clock, Duration> now) {
        return receive_impl([&limit, now] { return deadline_wait_error(limit, now); });
    }

    void close() noexcept {
        closed_ = true;
    }

    [[nodiscard]] std::size_t waiting_senders() const noexcept {
        return waiting_senders_;
    }

    [[nodiscard]] std::size_t waiting_receivers() const noexcept {
        return waiting_receivers_;
    }

private:
    [[nodiscard]] static std::optional<vio_error> cancellation_wait_error(
        const cancellation_token& token) {
        if (!token.cancellation_requested()) {
            return std::nullopt;
        }
        return make_error(vio_error_code::cancelled, "channel waiter cancelled");
    }

    template<class Clock, class Duration>
        requires(Clock::is_steady)
    [[nodiscard]] static std::optional<vio_error> deadline_wait_error(
        const deadline& limit,
        std::chrono::time_point<Clock, Duration> now) {
        if (!limit.expired(now)) {
            return std::nullopt;
        }
        return deadline::cancellation_error();
    }

    template<class WaitGuard>
    [[nodiscard]] void_result send_impl(T& value, WaitGuard before_wait) {
        if (closed_) {
            return std::unexpected(make_error(vio_error_code::closed));
        }
        if (capacity_ == 0) {
            if (!waiting_receivers_) {
                if (auto wait_error = before_wait(); wait_error.has_value()) {
                    return std::unexpected(std::move(*wait_error));
                }
                ++waiting_senders_;
                return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                                  "rendezvous receiver is not ready"));
            }
            --waiting_receivers_;
            rendezvous_value_ = std::move(value);
            return {};
        }
        if (buffer_.size() >= capacity_) {
            if (auto wait_error = before_wait(); wait_error.has_value()) {
                return std::unexpected(std::move(*wait_error));
            }
            ++waiting_senders_;
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "channel is full"));
        }
        buffer_.push_back(std::move(value));
        return {};
    }

    template<class WaitGuard>
    [[nodiscard]] io_result<T> receive_impl(WaitGuard before_wait) {
        if (rendezvous_value_.has_value()) {
            T value = std::move(*rendezvous_value_);
            rendezvous_value_.reset();
            return value;
        }
        if (!buffer_.empty()) {
            T value = std::move(buffer_.front());
            buffer_.pop_front();
            if (waiting_senders_ != 0) {
                --waiting_senders_;
            }
            return value;
        }
        if (closed_) {
            return std::unexpected(make_error(vio_error_code::closed));
        }
        if (auto wait_error = before_wait(); wait_error.has_value()) {
            return std::unexpected(std::move(*wait_error));
        }
        ++waiting_receivers_;
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "channel is empty"));
    }
    std::size_t capacity_;
    std::deque<T> buffer_;
    std::optional<T> rendezvous_value_;
    std::size_t waiting_senders_{0};
    std::size_t waiting_receivers_{0};
};

} // namespace voris::io
