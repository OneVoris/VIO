#include <voris/io/backends/io_uring_backend.hpp>

#include <iostream>

#if defined(__linux__)
#include <voris/io/backends/epoll_backend.hpp>

#include <array>
#include <chrono>
#include <cstddef>
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

[[nodiscard]] const char* platform_name() noexcept {
#if defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "other";
#endif
}

void emit_record(std::string_view backend,
                 std::string_view result,
                 std::string_view reason,
                 std::size_t rounds,
                 std::size_t operations,
                 std::int64_t elapsed_ns) {
    std::cout << "benchmark=backend_ping_pong"
              << " environment=" << platform_name()
              << " platform=" << platform_name()
              << " workload=" << workload
              << " backend=" << backend
              << " result=" << result
              << " reason=" << reason
              << " rounds=" << rounds
              << " operations=" << operations
              << " elapsed_ns=" << elapsed_ns << '\n';
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

[[nodiscard]] bool wait_for_completion(voris::io::backend& backend,
                                       std::size_t operation_id,
                                       std::string& reason) {
    std::array<voris::io::backend_completion, 4> completions{};
    for (std::size_t attempt = 0; attempt != 10000; ++attempt) {
        auto polled = backend.poll();
        if (!polled.has_value()) {
            reason = std::string("poll_") + error_name(polled.error().classification);
            return false;
        }

        auto drained = backend.drain_completions(completions);
        if (!drained.has_value()) {
            reason = std::string("drain_") + error_name(drained.error().classification);
            return false;
        }

        for (std::size_t index = 0; index != *drained; ++index) {
            const auto& completion = completions[index];
            if (completion.operation_id != operation_id) {
                continue;
            }
            if (!completion.result.has_value()) {
                reason = std::string("completion_") +
                         error_name(completion.result.error().classification);
                return false;
            }
            return true;
        }
        std::this_thread::yield();
    }

    reason = "timeout";
    return false;
}

[[nodiscard]] bool run_backend(std::string_view backend_name,
                               voris::io::backend& backend,
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
        if (!wait_for_completion(backend, operation_id, reason)) {
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
        if (!wait_for_completion(backend, operation_id, reason)) {
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
    (void)backend_name;
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
        if (run_backend("epoll", backend, rounds, operations, elapsed_ns, reason)) {
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
        if (run_backend("io_uring", backend, rounds, operations, elapsed_ns, reason)) {
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
