#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

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
        if (closed_) {
            return std::unexpected(make_error(vio_error_code::closed));
        }
        if (capacity_ == 0) {
            if (!waiting_receivers_) {
                ++waiting_senders_;
                return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                                  "rendezvous receiver is not ready"));
            }
            --waiting_receivers_;
            rendezvous_value_ = std::move(value);
            return {};
        }
        if (buffer_.size() >= capacity_) {
            ++waiting_senders_;
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "channel is full"));
        }
        buffer_.push_back(std::move(value));
        return {};
    }

    [[nodiscard]] io_result<T> receive() {
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
        ++waiting_receivers_;
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "channel is empty"));
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
    std::size_t capacity_;
    std::deque<T> buffer_;
    std::optional<T> rendezvous_value_;
    std::size_t waiting_senders_{0};
    std::size_t waiting_receivers_{0};
};

} // namespace voris::io
