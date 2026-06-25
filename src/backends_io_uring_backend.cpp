#include <voris/io/backends/io_uring_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/socket.h>
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
constexpr std::uint64_t cancel_user_data_tag = 1ULL << 63U;

struct decoded_kernel_user_data {
    std::size_t operation_id{};
    bool is_cancel{};
};

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

[[nodiscard]] bool is_socket_operation_kind(backend_operation_kind kind) noexcept {
    switch (kind) {
    case backend_operation_kind::read:
    case backend_operation_kind::write:
    case backend_operation_kind::accept:
    case backend_operation_kind::connect:
        return true;
    case backend_operation_kind::close:
    case backend_operation_kind::wake:
        return false;
    }

    return false;
}

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
        return false;
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

[[nodiscard]] vio_error provider_failure(int provider_code) {
    return make_error(vio_error_code::backend_failure, static_cast<std::int64_t>(provider_code));
}

[[nodiscard]] vio_error opcode_unavailable_error() {
    return make_error(vio_error_code::unsupported,
                      "io_uring operation opcode is unavailable");
}

[[nodiscard]] vio_error submission_queue_full_error() {
    return make_error(vio_error_code::resource_exhausted,
                      "io_uring submission queue is full");
}

[[nodiscard]] vio_error cancellation_required_error() {
    return make_error(vio_error_code::unsupported,
                      "io_uring socket operations require async cancel for close/shutdown liveness");
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

[[nodiscard]] io_result<unsigned> span_size_as_u32(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
        return std::unexpected(invalid_state_error("io_uring buffer size does not fit u32"));
    }
    return static_cast<unsigned>(size);
}

[[nodiscard]] io_result<int> native_handle_as_fd(std::size_t native_handle) {
    if (native_handle == 0 ||
        native_handle > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(invalid_state_error(
            "io_uring native handle must be a valid file descriptor"));
    }
    return static_cast<int>(native_handle);
}

[[nodiscard]] io_result<std::uint64_t> encode_operation_user_data(
    std::size_t operation_id) {
    const auto encoded = static_cast<std::uint64_t>(operation_id);
    if ((encoded & cancel_user_data_tag) != 0U) {
        return std::unexpected(invalid_state_error(
            "io_uring operation id must fit the kernel user_data operation tag"));
    }
    return encoded;
}

[[nodiscard]] io_result<std::uint64_t> encode_cancel_user_data(
    std::size_t operation_id) {
    auto encoded = encode_operation_user_data(operation_id);
    if (!encoded.has_value()) {
        return std::unexpected(encoded.error());
    }
    return *encoded | cancel_user_data_tag;
}

[[nodiscard]] decoded_kernel_user_data decode_kernel_user_data(
    std::uint64_t user_data) noexcept {
    return decoded_kernel_user_data{
        .operation_id = static_cast<std::size_t>(user_data & ~cancel_user_data_tag),
        .is_cancel = (user_data & cancel_user_data_tag) != 0U,
    };
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
    capabilities.supports_read = has_opcode(op_read);
    capabilities.supports_write = has_opcode(op_write);
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

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
struct io_uring_backend::kernel_ring {
    ~kernel_ring() {
        if (sqes_mapping != nullptr) {
            (void)::munmap(sqes_mapping, sqes_mapping_size);
        }
        if (cq_ring_mapping != nullptr && cq_ring_mapping != sq_ring_mapping) {
            (void)::munmap(cq_ring_mapping, cq_ring_mapping_size);
        }
        if (sq_ring_mapping != nullptr) {
            (void)::munmap(sq_ring_mapping, sq_ring_mapping_size);
        }
        if (fd >= 0) {
            (void)::close(fd);
        }
    }

    kernel_ring(const kernel_ring&) = delete;
    kernel_ring& operator=(const kernel_ring&) = delete;

    [[nodiscard]] static io_result<std::unique_ptr<kernel_ring>> create(
        std::size_t requested_entries) {
        if (requested_entries == 0 ||
            requested_entries > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
            return std::unexpected(invalid_state_error(
                "io_uring queue entries must fit unsigned"));
        }

        io_uring_params setup_params{};
        const long ring_fd =
            ::syscall(SYS_io_uring_setup, static_cast<unsigned>(requested_entries),
                      &setup_params);
        if (ring_fd < 0) {
            return std::unexpected(provider_failure(errno));
        }

        auto ring = std::unique_ptr<kernel_ring>(new kernel_ring());
        ring->fd = static_cast<int>(ring_fd);
        ring->params = setup_params;
        if (auto mapped = ring->map_queues(); !mapped.has_value()) {
            return std::unexpected(mapped.error());
        }
        return ring;
    }

    [[nodiscard]] io_result<io_uring_sqe*> acquire_sqe() {
        const auto head = std::atomic_ref<unsigned>(*sq_head).load();
        const auto tail = std::atomic_ref<unsigned>(*sq_tail).load();
        if (tail - head >= *sq_ring_entries) {
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "io_uring submission ring is full"));
        }

        const unsigned index = tail & *sq_ring_mask;
        auto* sqe = &sqes[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array[index] = index;
        std::atomic_ref<unsigned>(*sq_tail).store(tail + 1U);
        return sqe;
    }

    [[nodiscard]] void_result submit_one() {
        for (;;) {
            const long submitted = ::syscall(SYS_io_uring_enter, fd, 1U, 0U, 0U, nullptr, 0U);
            if (submitted == 1) {
                return {};
            }
            if (submitted < 0 && errno == EINTR) {
                continue;
            }
            if (submitted < 0) {
                return std::unexpected(provider_failure(errno));
            }
            return std::unexpected(make_error(vio_error_code::backend_failure,
                                              "io_uring submitted fewer SQEs than requested"));
        }
    }

    [[nodiscard]] std::size_t drain_cqes(std::span<io_uring_cqe> out) {
        const auto head = std::atomic_ref<unsigned>(*cq_head).load();
        const auto tail = std::atomic_ref<unsigned>(*cq_tail).load();
        const std::size_t available = static_cast<std::size_t>(tail - head);
        const std::size_t count = std::min(out.size(), available);

        for (std::size_t index = 0; index < count; ++index) {
            const unsigned cqe_index =
                (head + static_cast<unsigned>(index)) & *cq_ring_mask;
            out[index] = cqes[cqe_index];
        }
        if (count != 0) {
            std::atomic_ref<unsigned>(*cq_head).store(head + static_cast<unsigned>(count));
        }
        return count;
    }

    int fd{-1};
    io_uring_params params{};
    void* sq_ring_mapping{nullptr};
    void* cq_ring_mapping{nullptr};
    void* sqes_mapping{nullptr};
    std::size_t sq_ring_mapping_size{0};
    std::size_t cq_ring_mapping_size{0};
    std::size_t sqes_mapping_size{0};
    unsigned* sq_head{nullptr};
    unsigned* sq_tail{nullptr};
    unsigned* sq_ring_mask{nullptr};
    unsigned* sq_ring_entries{nullptr};
    unsigned* sq_array{nullptr};
    io_uring_sqe* sqes{nullptr};
    unsigned* cq_head{nullptr};
    unsigned* cq_tail{nullptr};
    unsigned* cq_ring_mask{nullptr};
    unsigned* cq_ring_entries{nullptr};
    io_uring_cqe* cqes{nullptr};

private:
    kernel_ring() = default;

    [[nodiscard]] void_result map_queues() {
        sq_ring_mapping_size =
            params.sq_off.array + params.sq_entries * sizeof(unsigned);
        cq_ring_mapping_size =
            params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0U) {
            sq_ring_mapping_size = std::max(sq_ring_mapping_size, cq_ring_mapping_size);
            cq_ring_mapping_size = sq_ring_mapping_size;
        }

        sq_ring_mapping = ::mmap(nullptr, sq_ring_mapping_size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, IORING_OFF_SQ_RING);
        if (sq_ring_mapping == MAP_FAILED) {
            sq_ring_mapping = nullptr;
            return std::unexpected(provider_failure(errno));
        }

        if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0U) {
            cq_ring_mapping = sq_ring_mapping;
        } else {
            cq_ring_mapping = ::mmap(nullptr, cq_ring_mapping_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, IORING_OFF_CQ_RING);
            if (cq_ring_mapping == MAP_FAILED) {
                cq_ring_mapping = nullptr;
                return std::unexpected(provider_failure(errno));
            }
        }

        sqes_mapping_size = params.sq_entries * sizeof(io_uring_sqe);
        sqes_mapping = ::mmap(nullptr, sqes_mapping_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, IORING_OFF_SQES);
        if (sqes_mapping == MAP_FAILED) {
            sqes_mapping = nullptr;
            return std::unexpected(provider_failure(errno));
        }

        bind_offsets();
        return {};
    }

    void bind_offsets() noexcept {
        auto* sq_base = static_cast<std::byte*>(sq_ring_mapping);
        auto* cq_base = static_cast<std::byte*>(cq_ring_mapping);
        sq_head = reinterpret_cast<unsigned*>(sq_base + params.sq_off.head);
        sq_tail = reinterpret_cast<unsigned*>(sq_base + params.sq_off.tail);
        sq_ring_mask = reinterpret_cast<unsigned*>(sq_base + params.sq_off.ring_mask);
        sq_ring_entries = reinterpret_cast<unsigned*>(sq_base + params.sq_off.ring_entries);
        sq_array = reinterpret_cast<unsigned*>(sq_base + params.sq_off.array);
        sqes = static_cast<io_uring_sqe*>(sqes_mapping);
        cq_head = reinterpret_cast<unsigned*>(cq_base + params.cq_off.head);
        cq_tail = reinterpret_cast<unsigned*>(cq_base + params.cq_off.tail);
        cq_ring_mask = reinterpret_cast<unsigned*>(cq_base + params.cq_off.ring_mask);
        cq_ring_entries = reinterpret_cast<unsigned*>(cq_base + params.cq_off.ring_entries);
        cqes = reinterpret_cast<io_uring_cqe*>(cq_base + params.cq_off.cqes);
    }
};
#else
struct io_uring_backend::kernel_ring {};
#endif

io_uring_backend::io_uring_backend(io_uring_capabilities capabilities,
                                   io_uring_backend_options options)
    : capabilities_(capabilities), options_(normalize_options(options)) {}

io_uring_backend::~io_uring_backend() = default;

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
    if (!is_socket_operation_kind(operation.kind)) {
        return std::unexpected(
            invalid_state_error("io_uring submit accepts socket operation kinds only"));
    }
    if (!supports_operation_kind(capabilities_, operation.kind)) {
        return std::unexpected(opcode_unavailable_error());
    }
    if (use_kernel_submission()) {
        if (!capabilities_.supports_cancel) {
            return std::unexpected(cancellation_required_error());
        }
        if (auto user_data = encode_operation_user_data(operation.id); !user_data.has_value()) {
            return std::unexpected(user_data.error());
        }
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
    submission_queue_.push_back(io_uring_backend::queued_submission{operation, std::nullopt});
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
    const auto queued = std::ranges::find_if(
        submission_queue_, [operation_id](const queued_submission& pending) {
            return pending.operation.id == operation_id;
    });
    if (queued != submission_queue_.end()) {
        if (use_kernel_submission()) {
            return std::unexpected(make_error(
                vio_error_code::unsupported,
                "io_uring queued cancellation is deferred to M6-005"));
        }
        if (!queued->cancellation.has_value()) {
            queued->cancellation = reason;
        }
        return {};
    }
    if (use_kernel_submission() && kernel_operations_.contains(operation_id)) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring active cancellation is deferred to M6-005"));
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
    if (use_kernel_submission()) {
        if (auto cancelled = request_kernel_cancellations_for(token); !cancelled.has_value()) {
            return cancelled;
        }
        complete_queued_submissions_for(token, closed_completion());
    } else {
        if (auto flushed = flush_queued_submissions_for(token); !flushed.has_value()) {
            return flushed;
        }
    }
    return fallback_.close_handle(token);
}

io_result<std::size_t> io_uring_backend::poll() {
    if (closed_) {
        if (use_kernel_submission() && has_inflight_kernel_work()) {
            return observe_completion_batch();
        }
        return std::unexpected(closed_error());
    }
    if (!capabilities_.available) {
        return std::unexpected(unavailable_error());
    }
    auto flushed = flush_submission_batch();
    if (!flushed.has_value()) {
        return std::unexpected(flushed.error());
    }
    // The count is newly observed completions made drainable by this poll.
    // Submission flushes only publish backend references and are not counted.
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

    if (use_kernel_submission()) {
        if (auto cancelled = request_kernel_cancellations_for({}); !cancelled.has_value()) {
            return cancelled;
        }
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
            backend_completion{submission_queue_.front().operation.id, closed_completion()});
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

bool io_uring_backend::use_kernel_submission() const noexcept {
    return options_.enable_kernel_submission;
}

bool io_uring_backend::has_inflight_kernel_work() const noexcept {
    return !kernel_operations_.empty() || !kernel_cancel_operation_ids_.empty();
}

void_result io_uring_backend::validate_kernel_socket_operation(
    const backend_operation& operation) const {
    if (auto fd = native_handle_as_fd(operation.handle.native_handle); !fd.has_value()) {
        return std::unexpected(fd.error());
    }

    switch (operation.kind) {
    case backend_operation_kind::read:
        if (auto size = span_size_as_u32(operation.read_buffer.size()); !size.has_value()) {
            return std::unexpected(size.error());
        }
        return {};
    case backend_operation_kind::write:
        if (auto size = span_size_as_u32(operation.write_buffer.size()); !size.has_value()) {
            return std::unexpected(size.error());
        }
        return {};
    case backend_operation_kind::accept:
        return {};
    case backend_operation_kind::connect:
        if (operation.socket_address.empty()) {
            return std::unexpected(
                invalid_state_error("io_uring connect requires a socket address"));
        }
#if defined(__linux__)
        if (operation.socket_address.size() >
            static_cast<std::size_t>(std::numeric_limits<socklen_t>::max())) {
            return std::unexpected(
                invalid_state_error("io_uring connect address does not fit socklen_t"));
        }
#endif
        return {};
    case backend_operation_kind::close:
    case backend_operation_kind::wake:
        return std::unexpected(
            invalid_state_error("io_uring kernel submission requires a socket operation"));
    }

    return std::unexpected(
        invalid_state_error("io_uring kernel submission requires a known operation"));
}

void_result io_uring_backend::submit_to_fallback(queued_submission queued) {
    auto submitted = fallback_.submit(queued.operation);
    if (!submitted.has_value()) {
        active_operation_ids_.erase(queued.operation.id);
        return submitted;
    }
    if (queued.cancellation.has_value()) {
        auto cancelled = fallback_.cancel(queued.operation.id, *queued.cancellation);
        if (!cancelled.has_value()) {
            return cancelled;
        }
    }
    return {};
}

void_result io_uring_backend::submit_to_kernel(const queued_submission& queued) {
    const backend_operation& operation = queued.operation;
    if (auto valid = validate_kernel_socket_operation(operation); !valid.has_value()) {
        return valid;
    }
    if (queued.cancellation.has_value()) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring queued cancellation is deferred to M6-005"));
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (!kernel_ring_) {
        auto created = kernel_ring::create(options_.submission_queue_capacity);
        if (!created.has_value()) {
            return std::unexpected(created.error());
        }
        kernel_ring_ = std::move(*created);
    }

    auto sqe_result = kernel_ring_->acquire_sqe();
    if (!sqe_result.has_value()) {
        return std::unexpected(sqe_result.error());
    }
    auto* sqe = *sqe_result;

    const auto fd = native_handle_as_fd(operation.handle.native_handle);
    if (!fd.has_value()) {
        return std::unexpected(fd.error());
    }
    sqe->fd = *fd;
    const auto user_data = encode_operation_user_data(operation.id);
    if (!user_data.has_value()) {
        return std::unexpected(user_data.error());
    }
    sqe->user_data = *user_data;

    switch (operation.kind) {
    case backend_operation_kind::read: {
        const auto size = span_size_as_u32(operation.read_buffer.size());
        if (!size.has_value()) {
            return std::unexpected(size.error());
        }
        sqe->opcode = IORING_OP_READ;
        sqe->off = 0;
        sqe->addr = reinterpret_cast<std::uintptr_t>(operation.read_buffer.data());
        sqe->len = *size;
        break;
    }
    case backend_operation_kind::write: {
        const auto size = span_size_as_u32(operation.write_buffer.size());
        if (!size.has_value()) {
            return std::unexpected(size.error());
        }
        sqe->opcode = IORING_OP_WRITE;
        sqe->off = 0;
        sqe->addr = reinterpret_cast<std::uintptr_t>(operation.write_buffer.data());
        sqe->len = *size;
        break;
    }
    case backend_operation_kind::accept:
        sqe->opcode = IORING_OP_ACCEPT;
        sqe->addr = 0;
        sqe->addr2 = 0;
        sqe->len = 0;
        sqe->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        break;
    case backend_operation_kind::connect:
        sqe->opcode = IORING_OP_CONNECT;
        sqe->off = operation.socket_address.size();
        sqe->addr = reinterpret_cast<std::uintptr_t>(operation.socket_address.data());
        sqe->len = 0;
        break;
    case backend_operation_kind::close:
    case backend_operation_kind::wake:
        return std::unexpected(
            invalid_state_error("io_uring kernel submission requires a socket operation"));
    }

    kernel_operations_.emplace(operation.id,
                               io_uring_backend::kernel_operation{.operation = operation});
    if (auto submitted = kernel_ring_->submit_one(); !submitted.has_value()) {
        kernel_operations_.erase(operation.id);
        completion_queue_.push_back(
            backend_completion{operation.id, std::unexpected(submitted.error())});
    }
    return {};
#else
    (void)operation;
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::request_kernel_cancellations_for(
    backend_handle_token token) {
    if (kernel_operations_.empty()) {
        return {};
    }
    if (!capabilities_.supports_cancel) {
        return std::unexpected(cancellation_required_error());
    }

    for (auto& [operation_id, operation] : kernel_operations_) {
        if (token.generation != 0 && operation.operation.handle != token) {
            continue;
        }

        if (operation.cancel_submitted) {
            operation.close_requested = true;
            continue;
        }

        if (auto cancelled = request_kernel_cancellation_for(operation_id, operation);
            !cancelled.has_value()) {
            return cancelled;
        }
        operation.close_requested = true;
    }

    return {};
}

void_result io_uring_backend::request_kernel_cancellation_for(
    std::size_t operation_id,
    kernel_operation& operation) {
#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (!kernel_ring_) {
        return std::unexpected(
            invalid_state_error("io_uring kernel cancellation requires an active ring"));
    }

    const auto target_user_data = encode_operation_user_data(operation_id);
    if (!target_user_data.has_value()) {
        return std::unexpected(target_user_data.error());
    }
    const auto cancel_user_data = encode_cancel_user_data(operation_id);
    if (!cancel_user_data.has_value()) {
        return std::unexpected(cancel_user_data.error());
    }

    auto sqe_result = kernel_ring_->acquire_sqe();
    if (!sqe_result.has_value()) {
        return std::unexpected(sqe_result.error());
    }

    auto* sqe = *sqe_result;
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = -1;
    sqe->addr = *target_user_data;
    sqe->cancel_flags = 0;
    sqe->user_data = *cancel_user_data;

    if (auto submitted = kernel_ring_->submit_one(); !submitted.has_value()) {
        return std::unexpected(submitted.error());
    }

    operation.cancel_submitted = true;
    kernel_cancel_operation_ids_.insert(operation_id);
    return {};
#else
    (void)operation_id;
    (void)operation;
    return std::unexpected(unavailable_error());
#endif
}

io_result<std::size_t> io_uring_backend::flush_submission_batch() {
    const auto count = std::min(options_.submit_batch_limit, submission_queue_.size());
    std::size_t flushed = 0;
    for (; flushed < count; ++flushed) {
        auto queued = submission_queue_.front();
        auto submitted = use_kernel_submission() ? submit_to_kernel(queued)
                                                 : submit_to_fallback(std::move(queued));
        if (!submitted.has_value()) {
            return std::unexpected(submitted.error());
        }
        submission_queue_.pop_front();
    }
    return flushed;
}

io_result<std::size_t> io_uring_backend::observe_completion_batch() {
    if (use_kernel_submission()) {
        return observe_kernel_completions();
    }

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

io_result<std::size_t> io_uring_backend::observe_kernel_completions() {
#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (!kernel_ring_) {
        return 0U;
    }

    std::vector<io_uring_cqe> batch(options_.completion_batch_limit);
    const std::size_t count = kernel_ring_->drain_cqes(batch);
    std::size_t visible = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& cqe = batch[index];
        const auto decoded = decode_kernel_user_data(cqe.user_data);
        if (decoded.is_cancel) {
            kernel_cancel_operation_ids_.erase(decoded.operation_id);
            if (auto found = kernel_operations_.find(decoded.operation_id);
                found != kernel_operations_.end()) {
                found->second.cancel_submitted = false;
            }
            continue;
        }

        const auto operation_id = decoded.operation_id;
        const auto found = kernel_operations_.find(operation_id);
        if (found == kernel_operations_.end()) {
            continue;
        }

        const auto operation = found->second;
        kernel_operations_.erase(found);

        backend_completion completion{};
        completion.operation_id = operation_id;
        if (operation.close_requested ||
            !fallback_.is_current_handle(operation.operation.handle)) {
            if (operation.operation.kind == backend_operation_kind::accept && cqe.res >= 0) {
                (void)::close(cqe.res);
            }
            completion.result = closed_completion();
        } else if (cqe.res < 0) {
            completion.result = std::unexpected(provider_failure(-cqe.res));
        } else {
            switch (operation.operation.kind) {
            case backend_operation_kind::read:
            case backend_operation_kind::write:
                completion.bytes_transferred = static_cast<std::size_t>(cqe.res);
                break;
            case backend_operation_kind::accept:
                if (cqe.res == 0) {
                    const int duplicate = ::fcntl(cqe.res, F_DUPFD_CLOEXEC, 1);
                    if (duplicate < 0) {
                        completion.result = std::unexpected(provider_failure(errno));
                        (void)::close(cqe.res);
                        break;
                    }
                    (void)::close(cqe.res);
                    completion.accepted_native_handle = static_cast<std::size_t>(duplicate);
                } else {
                    completion.accepted_native_handle = static_cast<std::size_t>(cqe.res);
                }
                break;
            case backend_operation_kind::connect:
                break;
            case backend_operation_kind::close:
            case backend_operation_kind::wake:
                completion.result = std::unexpected(invalid_state_error(
                    "unexpected non-socket io_uring completion"));
                break;
            }
        }

        completion_queue_.push_back(std::move(completion));
        ++visible;
    }
    return visible;
#else
    return std::unexpected(unavailable_error());
#endif
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
        if (iterator->operation.handle == token) {
            auto queued = std::move(*iterator);
            iterator = submission_queue_.erase(iterator);

            auto submitted = submit_to_fallback(std::move(queued));
            if (!submitted.has_value()) {
                return submitted;
            }
            continue;
        }
        ++iterator;
    }
    return {};
}

void io_uring_backend::complete_queued_submissions_for(backend_handle_token token,
                                                       const void_result& result) {
    for (auto iterator = submission_queue_.begin(); iterator != submission_queue_.end();) {
        if (token.generation == 0 || iterator->operation.handle == token) {
            completion_queue_.push_back(
                backend_completion{iterator->operation.id, result});
            iterator = submission_queue_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

} // namespace voris::io::backends
