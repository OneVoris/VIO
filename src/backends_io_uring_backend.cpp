#include <voris/io/backends/io_uring_backend.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace voris::io::backends {

namespace detail {

[[nodiscard]] io_uring_capabilities capabilities_from_io_uring_probe_opcodes(
    std::span<const unsigned> supported_opcodes) noexcept;

} // namespace detail

namespace {

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

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_register)

constexpr unsigned register_probe = 8U;
constexpr unsigned op_supported = 1U << 0U;
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

void apply_probe(io_uring_capabilities& capabilities,
                 const kernel_probe_buffer& probe) noexcept {
    std::array<unsigned, probe_op_capacity> supported_opcodes{};
    std::size_t supported_count{0};

    const auto count = probe.ops_len < probe.ops.size() ? probe.ops_len : probe.ops.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto& op = probe.ops[index];
        if (op.op <= probe.last_op && (op.flags & op_supported) != 0U) {
            supported_opcodes[supported_count++] = op.op;
        }
    }

    capabilities = detail::capabilities_from_io_uring_probe_opcodes(
        std::span<const unsigned>{supported_opcodes.data(), supported_count});
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

[[nodiscard]] vio_error closed_error() {
    return make_error(vio_error_code::closed);
}

[[nodiscard]] vio_error invalid_state_error(std::string diagnostic) {
    return make_error(vio_error_code::invalid_state, std::move(diagnostic));
}

[[nodiscard]] vio_error opcode_unavailable_error() {
    return make_error(vio_error_code::unsupported,
                      "io_uring operation opcode is unavailable");
}

[[nodiscard]] vio_error submission_queue_full_error() {
    return make_error(vio_error_code::resource_exhausted,
                      "io_uring submission queue is full");
}

[[nodiscard]] void_result closed_completion() {
    return std::unexpected(make_error(vio_error_code::closed));
}

[[nodiscard]] io_uring_backend_options normalize_options(
    io_uring_backend_options options) noexcept {
    if (options.submit_batch_limit == 0) {
        options.submit_batch_limit = 1;
    }
    if (options.completion_batch_limit == 0) {
        options.completion_batch_limit = 1;
    }
    return options;
}

} // namespace

namespace detail {

io_uring_capabilities capabilities_from_io_uring_probe_opcodes(
    std::span<const unsigned> supported_opcodes) noexcept {
    const auto has_opcode = [supported_opcodes](unsigned opcode) noexcept {
        for (const auto supported : supported_opcodes) {
            if (supported == opcode) {
                return true;
            }
        }
        return false;
    };

    io_uring_capabilities capabilities{.available = true};
    capabilities.supports_read = has_opcode(op_read) || has_opcode(op_readv);
    capabilities.supports_write = has_opcode(op_write) || has_opcode(op_writev);
    capabilities.supports_accept = has_opcode(op_accept);
    capabilities.supports_connect = has_opcode(op_connect);
    capabilities.supports_fsync = has_opcode(op_fsync);
    capabilities.supports_cancel = has_opcode(op_async_cancel);
    capabilities.supports_files = capabilities.supports_read &&
                                  capabilities.supports_write &&
                                  capabilities.supports_fsync;
    capabilities.supports_registered_buffers =
        has_opcode(op_read_fixed) && has_opcode(op_write_fixed);
    capabilities.supports_registered_files = has_opcode(op_files_update);
    return capabilities;
}

} // namespace detail

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

io_uring_backend::io_uring_backend(io_uring_capabilities capabilities,
                                   io_uring_backend_options options)
    : capabilities_(capabilities), options_(normalize_options(options)) {}

const io_uring_capabilities& io_uring_backend::capabilities() const noexcept {
    return capabilities_;
}

io_uring_backend_state io_uring_backend::state() const noexcept {
    if (closed_) {
        return io_uring_backend_state::closed;
    }
    if (!capabilities_.available) {
        return io_uring_backend_state::unavailable;
    }
    return io_uring_backend_state::active;
}

bool io_uring_backend::default_eligible() const noexcept {
    return capabilities_.available && capabilities_.supports_read &&
           capabilities_.supports_write && capabilities_.supports_accept &&
           capabilities_.supports_connect && capabilities_.supports_files &&
           capabilities_.supports_fsync && capabilities_.supports_cancel;
}

io_result<backend_handle_token> io_uring_backend::register_handle(std::size_t native_handle) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.register_handle(native_handle);
}

void_result io_uring_backend::submit(backend_operation operation) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    if (!supports_operation_kind(capabilities_, operation.kind)) {
        return std::unexpected(opcode_unavailable_error());
    }
    if (operation.id == 0) {
        return std::unexpected(invalid_state_error("operation id must be non-zero"));
    }
    if (!fallback_.is_current_handle(operation.handle)) {
        return std::unexpected(
            invalid_state_error("operation handle token is not current"));
    }
    if (active_operation_ids_.contains(operation.id)) {
        return std::unexpected(invalid_state_error("operation id is already active"));
    }
    if (submission_queue_.size() >= options_.submission_queue_capacity) {
        return std::unexpected(submission_queue_full_error());
    }

    active_operation_ids_.insert(operation.id);
    submission_queue_.push_back(operation);
    return {};
}

void_result io_uring_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
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
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    if (!fallback_.is_current_handle(token)) {
        return std::unexpected(invalid_state_error("backend handle token is not current"));
    }
    if (auto flushed = flush_queued_submissions_for(token); !flushed.has_value()) {
        return flushed;
    }
    return fallback_.close_handle(token);
}

io_result<std::size_t> io_uring_backend::poll() {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    auto flushed = flush_submission_batch();
    if (!flushed.has_value()) {
        return std::unexpected(flushed.error());
    }
    return observe_completion_batch();
}

io_result<std::size_t> io_uring_backend::drain_completions(
    std::span<backend_completion> out) {
    if (out.empty()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "completion drain output must not be empty"));
    }
    if (!closed_ && !capabilities_.available) {
        return std::unexpected(unavailable_error());
    }

    const auto count = std::min(out.size(), completion_queue_.size());
    for (std::size_t index = 0; index < count; ++index) {
        active_operation_ids_.erase(completion_queue_.front().operation_id);
        out[index] = std::move(completion_queue_.front());
        completion_queue_.pop_front();
    }
    return count;
}

void_result io_uring_backend::wake() {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    return fallback_.wake();
}

void_result io_uring_backend::shutdown() {
    if (closed_) {
        return {};
    }

    closed_ = true;
    if (auto stopped = fallback_.shutdown(); !stopped.has_value()) {
        return stopped;
    }
    if (auto drained = drain_fallback_completions(); !drained.has_value()) {
        return drained;
    }

    while (!submission_queue_.empty()) {
        completion_queue_.push_back(
            backend_completion{submission_queue_.front().id, closed_completion()});
        submission_queue_.pop_front();
    }

    return {};
}

void_result io_uring_backend::register_buffers(std::size_t count) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available || !capabilities_.supports_registered_buffers) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered buffers unavailable"));
    }
    registered_buffers_ = count;
    return {};
}

void_result io_uring_backend::register_files(std::size_t count) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available || !capabilities_.supports_registered_files) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered files unavailable"));
    }
    registered_files_ = count;
    return {};
}

io_result<std::size_t> io_uring_backend::flush_submission_batch() {
    const auto count = std::min(options_.submit_batch_limit, submission_queue_.size());
    std::size_t flushed = 0;
    for (; flushed < count; ++flushed) {
        auto operation = submission_queue_.front();
        submission_queue_.pop_front();

        auto submitted = fallback_.submit(operation);
        if (!submitted.has_value()) {
            active_operation_ids_.erase(operation.id);
            return std::unexpected(submitted.error());
        }
    }
    return flushed;
}

io_result<std::size_t> io_uring_backend::observe_completion_batch() {
    auto available = fallback_.poll();
    if (!available.has_value()) {
        return std::unexpected(available.error());
    }

    const auto count = std::min(*available, options_.completion_batch_limit);
    if (count == 0) {
        return 0U;
    }

    std::vector<backend_completion> batch(count);
    auto drained = fallback_.drain_completions(
        std::span<backend_completion>{batch.data(), batch.size()});
    if (!drained.has_value()) {
        return std::unexpected(drained.error());
    }

    for (std::size_t index = 0; index < *drained; ++index) {
        completion_queue_.push_back(std::move(batch[index]));
    }
    return *drained;
}

void_result io_uring_backend::drain_fallback_completions() {
    std::array<backend_completion, 32> batch{};
    for (;;) {
        auto drained = fallback_.drain_completions(
            std::span<backend_completion>{batch.data(), batch.size()});
        if (!drained.has_value()) {
            return std::unexpected(drained.error());
        }
        if (*drained == 0) {
            return {};
        }
        for (std::size_t index = 0; index < *drained; ++index) {
            completion_queue_.push_back(std::move(batch[index]));
        }
    }
}

void_result io_uring_backend::flush_queued_submissions_for(backend_handle_token token) {
    for (auto iterator = submission_queue_.begin(); iterator != submission_queue_.end();) {
        if (iterator->handle == token) {
            const auto operation = *iterator;
            iterator = submission_queue_.erase(iterator);

            auto submitted = fallback_.submit(operation);
            if (!submitted.has_value()) {
                active_operation_ids_.erase(operation.id);
                return submitted;
            }
            continue;
        }
        ++iterator;
    }
    return {};
}

} // namespace voris::io::backends
