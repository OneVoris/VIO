#include <voris/io/backends/io_uring_backend.hpp>

#include "benchmark_support.hpp"

#include <iostream>

#if defined(__linux__)
#include <voris/io/backends/epoll_backend.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr std::string_view workload = "socketpair_ping_pong";

void emit_record(std::string_view backend,
                 std::string_view result,
                 std::string_view reason,
                 std::size_t rounds,
                 std::size_t operations,
                 std::int64_t elapsed_ns) {
    const bool failed = result == "failed";
    const bool timeout = reason.find("timeout") != std::string_view::npos;
    vio_bench::record record{
        .benchmark = "backend_ping_pong",
        .workload = workload,
        .result = result,
        .reason = reason,
        .operations = operations,
        .elapsed_ns = elapsed_ns,
        .errors = failed ? 1U : 0U,
        .timeouts = timeout ? 1U : 0U,
        .extra = {{"backend", std::string(backend)},
                  {"rounds", std::to_string(rounds)}}};
    vio_bench::emit(record);
}

#if defined(__linux__)
class unique_fd {
public:
    explicit unique_fd(int fd = -1) noexcept : fd_(fd) {}

    ~unique_fd() {
        reset();
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    unique_fd(unique_fd&& other) noexcept : fd_(other.release()) {}

    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

[[nodiscard]] std::array<unique_fd, 2> make_socket_pair() {
    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     fds.data()) != 0) {
        return {};
    }
    return {unique_fd(fds[0]), unique_fd(fds[1])};
}

[[nodiscard]] std::string error_name(voris::io::vio_error_code code) {
    return std::string(voris::io::to_string(code));
}

voris::io::backend_operation read_operation(
    std::size_t id,
    voris::io::backend_handle_token token,
    std::span<std::byte> buffer) {
    voris::io::backend_operation operation{};
    operation.id = id;
    operation.kind = voris::io::backend_operation_kind::read;
    operation.handle = token;
    operation.read_buffer = buffer;
    return operation;
}

voris::io::backend_operation write_operation(
    std::size_t id,
    voris::io::backend_handle_token token,
    std::span<const std::byte> buffer) {
    voris::io::backend_operation operation{};
    operation.id = id;
    operation.kind = voris::io::backend_operation_kind::write;
    operation.handle = token;
    operation.write_buffer = buffer;
    return operation;
}

[[nodiscard]] std::optional<voris::io::backend_completion> wait_for_completion(
    voris::io::backend& backend,
    std::size_t operation_id,
    std::string& reason) {
    std::array<voris::io::backend_completion, 4> completions{};
    for (std::size_t attempt = 0; attempt != 10000; ++attempt) {
        auto polled = backend.poll();
        if (!polled.has_value()) {
            reason = std::string("poll_") + error_name(polled.error().classification);
            return std::nullopt;
        }

        auto drained = backend.drain_completions(completions);
        if (!drained.has_value()) {
            reason = std::string("drain_") + error_name(drained.error().classification);
            return std::nullopt;
        }

        for (std::size_t index = 0; index != *drained; ++index) {
            const auto& completion = completions[index];
            if (completion.operation_id != operation_id) {
                continue;
            }
            if (!completion.result.has_value()) {
                reason = std::string("completion_") +
                         error_name(completion.result.error().classification);
                return std::nullopt;
            }
            return completion;
        }
        std::this_thread::yield();
    }

    reason = "timeout";
    return std::nullopt;
}

[[nodiscard]] bool send_payload(int fd,
                                std::span<const std::byte> payload,
                                std::string& reason) {
    for (;;) {
        const auto sent = ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(payload.size())) {
            return true;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            reason = std::string("send_errno_") + std::to_string(errno);
            return false;
        }
        reason = "send_short";
        return false;
    }
}

[[nodiscard]] bool recv_payload(int fd,
                                std::span<std::byte> output,
                                std::string& reason) {
    for (;;) {
        const auto received = ::recv(fd, output.data(), output.size(), 0);
        if (received == static_cast<ssize_t>(output.size())) {
            return true;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            reason = std::string("recv_errno_") + std::to_string(errno);
            return false;
        }
        reason = "recv_short";
        return false;
    }
}

[[nodiscard]] bool run_epoll_backend(voris::io::backends::epoll_backend& backend,
                                     std::size_t rounds,
                                     std::size_t& operations,
                                     std::int64_t& elapsed_ns,
                                     std::string& reason) {
    auto sockets = make_socket_pair();
    if (sockets[0].get() < 0 || sockets[1].get() < 0) {
        reason = "socketpair_failed";
        return false;
    }

    auto writer = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    if (!writer.has_value()) {
        reason = std::string("register_writer_") +
                 error_name(writer.error().classification);
        return false;
    }
    auto reader = backend.register_handle(static_cast<std::size_t>(sockets[1].get()));
    if (!reader.has_value()) {
        reason = std::string("register_reader_") +
                 error_name(reader.error().classification);
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    std::size_t operation_id = 1;
    for (std::size_t round = 0; round != rounds; ++round) {
        const std::array payload{std::byte{0x76}};
        std::array<std::byte, 1> output{};

        auto submitted_write =
            backend.submit(write_operation(operation_id, *writer, payload));
        if (!submitted_write.has_value()) {
            reason = std::string("submit_write_") +
                     error_name(submitted_write.error().classification);
            return false;
        }
        if (!wait_for_completion(backend, operation_id, reason).has_value()) {
            reason = std::string("write_") + reason;
            return false;
        }
        if (!send_payload(sockets[0].get(), payload, reason)) {
            reason = std::string("write_") + reason;
            return false;
        }
        ++operation_id;

        auto submitted_read =
            backend.submit(read_operation(operation_id, *reader, output));
        if (!submitted_read.has_value()) {
            reason = std::string("submit_read_") +
                     error_name(submitted_read.error().classification);
            return false;
        }
        if (!wait_for_completion(backend, operation_id, reason).has_value()) {
            reason = std::string("read_") + reason;
            return false;
        }
        if (!recv_payload(sockets[1].get(), output, reason)) {
            reason = std::string("read_") + reason;
            return false;
        }
        if (output != payload) {
            reason = "payload_mismatch";
            return false;
        }
        ++operation_id;
    }
    const auto finished = std::chrono::steady_clock::now();

    operations = rounds * 2U;
    elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
            .count();
    (void)backend.shutdown();
    return true;
}

[[nodiscard]] bool run_io_uring_backend(
    voris::io::backends::io_uring_backend& backend,
    std::size_t rounds,
    std::size_t& operations,
    std::int64_t& elapsed_ns,
    std::string& reason) {
    auto sockets = make_socket_pair();
    if (sockets[0].get() < 0 || sockets[1].get() < 0) {
        reason = "socketpair_failed";
        return false;
    }

    auto writer = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    if (!writer.has_value()) {
        reason = std::string("register_writer_") +
                 error_name(writer.error().classification);
        return false;
    }
    auto reader = backend.register_handle(static_cast<std::size_t>(sockets[1].get()));
    if (!reader.has_value()) {
        reason = std::string("register_reader_") +
                 error_name(reader.error().classification);
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    std::size_t operation_id = 1;
    for (std::size_t round = 0; round != rounds; ++round) {
        const std::array payload{std::byte{0x76}};
        std::array<std::byte, 1> output{};

        auto submitted_write =
            backend.submit(write_operation(operation_id, *writer, payload));
        if (!submitted_write.has_value()) {
            reason = std::string("submit_write_") +
                     error_name(submitted_write.error().classification);
            return false;
        }
        auto write_completion = wait_for_completion(backend, operation_id, reason);
        if (!write_completion.has_value()) {
            reason = std::string("write_") + reason;
            return false;
        }
        const bool write_size_matches =
            write_completion->bytes_transferred == payload.size();
        if (!write_size_matches) {
            reason = "write_short_completion";
            return false;
        }
        ++operation_id;

        auto submitted_read =
            backend.submit(read_operation(operation_id, *reader, output));
        if (!submitted_read.has_value()) {
            reason = std::string("submit_read_") +
                     error_name(submitted_read.error().classification);
            return false;
        }
        auto read_completion = wait_for_completion(backend, operation_id, reason);
        if (!read_completion.has_value()) {
            reason = std::string("read_") + reason;
            return false;
        }
        const bool read_size_matches =
            read_completion->bytes_transferred == payload.size();
        if (!read_size_matches) {
            reason = "read_short_completion";
            return false;
        }
        if (output != payload) {
            reason = "payload_mismatch";
            return false;
        }
        ++operation_id;
    }
    const auto finished = std::chrono::steady_clock::now();

    operations = rounds * 2U;
    elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
            .count();
    (void)backend.shutdown();
    return true;
}
#endif

} // namespace

int main() {
#if defined(__linux__)
    constexpr std::size_t rounds = 1000;

    {
        voris::io::backends::epoll_backend backend;
        std::size_t operations{};
        std::int64_t elapsed_ns{};
        std::string reason{"ok"};
        if (run_epoll_backend(backend, rounds, operations, elapsed_ns, reason)) {
            emit_record("epoll", "ok", "ok", rounds, operations, elapsed_ns);
        } else {
            emit_record("epoll", "failed", reason, rounds, operations, elapsed_ns);
            return 1;
        }
    }

    const auto capabilities = voris::io::backends::detect_io_uring_capabilities();
    const voris::io::backends::io_uring_backend capability_probe(capabilities);
    if (!capability_probe.default_eligible()) {
        emit_record("io_uring", "skipped", "missing_core_capabilities", 0, 0, 0);
        return 0;
    }

    {
        voris::io::backends::io_uring_backend backend(capabilities);
        std::size_t operations{};
        std::int64_t elapsed_ns{};
        std::string reason{"ok"};
        if (run_io_uring_backend(backend, rounds, operations, elapsed_ns, reason)) {
            emit_record("io_uring", "ok", "ok", rounds, operations, elapsed_ns);
            return 0;
        }

        emit_record("io_uring", "failed", reason, rounds, operations, elapsed_ns);
        return 1;
    }
#else
    emit_record("epoll", "skipped", "non_linux", 0, 0, 0);
    emit_record("io_uring", "skipped", "non_linux", 0, 0, 0);
    return 0;
#endif
}
