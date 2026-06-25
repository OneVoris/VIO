#include <voris/io/backends/io_uring_backend.hpp>

#include <array>
#include <cstdint>

#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace voris::io::backends {

namespace {

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_register)

constexpr unsigned register_probe = 8U;
constexpr unsigned op_supported = 1U << 0U;
constexpr unsigned op_readv = 1U;
constexpr unsigned op_writev = 2U;
constexpr unsigned op_fsync = 3U;
constexpr unsigned op_read_fixed = 4U;
constexpr unsigned op_write_fixed = 5U;
constexpr unsigned op_accept = 13U;
constexpr unsigned op_async_cancel = 14U;
constexpr unsigned op_connect = 16U;
constexpr unsigned op_files_update = 20U;
constexpr unsigned op_read = 22U;
constexpr unsigned op_write = 23U;
constexpr std::uint32_t minimal_ring_entries = 2U;
constexpr std::size_t probe_op_capacity = 64U;

struct kernel_probe_op {
    std::uint8_t op{};
    std::uint8_t resv{};
    std::uint16_t flags{};
    std::uint32_t resv2{};
};

struct kernel_probe_buffer {
    std::uint8_t last_op{};
    std::uint8_t ops_len{};
    std::uint16_t resv{};
    std::uint32_t resv2[3]{};
    std::array<kernel_probe_op, probe_op_capacity> ops{};
};

class unique_fd {
public:
    explicit unique_fd(int fd = -1) noexcept : fd_(fd) {}

    ~unique_fd() {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

private:
    int fd_{-1};
};

[[nodiscard]] bool probe_supports(const kernel_probe_buffer& probe,
                                  unsigned opcode) noexcept {
    if (opcode > probe.last_op) {
        return false;
    }

    const auto count = probe.ops_len < probe.ops.size() ? probe.ops_len : probe.ops.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto& op = probe.ops[index];
        if (op.op == opcode) {
            return (op.flags & op_supported) != 0U;
        }
    }

    return false;
}

void apply_probe(io_uring_capabilities& capabilities,
                 const kernel_probe_buffer& probe) noexcept {
    capabilities.supports_read =
        probe_supports(probe, op_read) || probe_supports(probe, op_readv);
    capabilities.supports_write =
        probe_supports(probe, op_write) || probe_supports(probe, op_writev);
    capabilities.supports_accept = probe_supports(probe, op_accept);
    capabilities.supports_connect = probe_supports(probe, op_connect);
    capabilities.supports_fsync = probe_supports(probe, op_fsync);
    capabilities.supports_cancel = probe_supports(probe, op_async_cancel);
    capabilities.supports_files = capabilities.supports_read &&
                                  capabilities.supports_write &&
                                  capabilities.supports_fsync;
    capabilities.supports_registered_buffers =
        probe_supports(probe, op_read_fixed) && probe_supports(probe, op_write_fixed);
    capabilities.supports_registered_files = probe_supports(probe, op_files_update);
}

#endif

[[nodiscard]] bool supports_operation_kind(
    const io_uring_capabilities& capabilities,
    backend_operation_kind kind) noexcept {
    switch (kind) {
    case backend_operation_kind::read:
        return capabilities.supports_read;
    case backend_operation_kind::write:
        return capabilities.supports_write;
    case backend_operation_kind::accept:
        return capabilities.supports_accept;
    case backend_operation_kind::connect:
        return capabilities.supports_connect;
    case backend_operation_kind::close:
    case backend_operation_kind::wake:
        return true;
    }

    return false;
}

[[nodiscard]] vio_error unavailable_error() {
    return make_error(vio_error_code::unsupported, "io_uring is unavailable");
}

[[nodiscard]] vio_error opcode_unavailable_error() {
    return make_error(vio_error_code::unsupported,
                      "io_uring operation opcode is unavailable");
}

} // namespace

io_uring_capabilities detect_io_uring_capabilities() noexcept {
#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_register)
    io_uring_params params{};
    const long fd =
        ::syscall(SYS_io_uring_setup, minimal_ring_entries, &params);
    if (fd < 0) {
        return {};
    }

    const unique_fd ring(static_cast<int>(fd));
    io_uring_capabilities capabilities{.available = true};

    kernel_probe_buffer probe{};
    const long probe_result =
        ::syscall(SYS_io_uring_register, ring.get(), register_probe, &probe,
                  probe_op_capacity);
    if (probe_result >= 0) {
        apply_probe(capabilities, probe);
    }

    return capabilities;
#else
    return {};
#endif
}

io_uring_backend::io_uring_backend(io_uring_capabilities capabilities)
    : capabilities_(capabilities) {}

const io_uring_capabilities& io_uring_backend::capabilities() const noexcept {
    return capabilities_;
}

bool io_uring_backend::default_eligible() const noexcept {
    return capabilities_.available && capabilities_.supports_read &&
           capabilities_.supports_write && capabilities_.supports_accept &&
           capabilities_.supports_connect && capabilities_.supports_files &&
           capabilities_.supports_fsync && capabilities_.supports_cancel;
}

io_result<backend_handle_token> io_uring_backend::register_handle(std::size_t native_handle) {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.register_handle(native_handle);
}

void_result io_uring_backend::submit(backend_operation operation) {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    if (!supports_operation_kind(capabilities_, operation.kind)) {
        return std::unexpected(opcode_unavailable_error());
    }
    return fallback_.submit(operation);
}

void_result io_uring_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    if (!capabilities_.supports_cancel) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring cancellation is unavailable"));
    }
    return fallback_.cancel(operation_id, reason);
}

void_result io_uring_backend::close_handle(backend_handle_token token) {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.close_handle(token);
}

io_result<std::size_t> io_uring_backend::poll() {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.poll();
}

io_result<std::size_t> io_uring_backend::drain_completions(
    std::span<backend_completion> out) {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.drain_completions(out);
}

void_result io_uring_backend::wake() {
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.wake();
}

void_result io_uring_backend::shutdown() {
    return fallback_.shutdown();
}

void_result io_uring_backend::register_buffers(std::size_t count) {
    if (!capabilities_.available || !capabilities_.supports_registered_buffers) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered buffers unavailable"));
    }
    registered_buffers_ = count;
    return {};
}

void_result io_uring_backend::register_files(std::size_t count) {
    if (!capabilities_.available || !capabilities_.supports_registered_files) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered files unavailable"));
    }
    registered_files_ = count;
    return {};
}

} // namespace voris::io::backends
