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
#include <sys/uio.h>
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
// io_uring CQEs report Linux errno values; keep the helper testable off Linux.
constexpr int linux_ecanceled = 125;

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

[[nodiscard]] bool supports_operation(
    const io_uring_capabilities& capabilities,
    const backend_operation& operation) noexcept {
    switch (operation.kind) {
    case backend_operation_kind::read:
        return capabilities.supports_read;
    case backend_operation_kind::write:
        return capabilities.supports_write;
    case backend_operation_kind::accept:
        return capabilities.supports_accept;
    case backend_operation_kind::connect:
        return capabilities.supports_connect;
    case backend_operation_kind::fsync:
        return capabilities.supports_fsync;
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
                      "io_uring kernel operations require async cancel for close/shutdown liveness");
}

[[nodiscard]] void_result closed_completion() {
    return std::unexpected(make_error(vio_error_code::closed));
}

[[nodiscard]] void_result cancelled_completion(cancellation_reason reason) {
    return std::unexpected(make_error(vio_error_code::cancelled,
                                      std::string("io_uring operation cancelled: ") +
                                          std::string(to_string(reason))));
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

[[nodiscard]] io_result<unsigned> count_as_u32(std::size_t count,
                                               std::string diagnostic) {
    if (count > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
        return std::unexpected(invalid_state_error(std::move(diagnostic)));
    }
    return static_cast<unsigned>(count);
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

void discard_submitted_io_uring_submissions(
    std::deque<pending_io_uring_submission>& pending,
    unsigned submitted_count) noexcept {
    while (submitted_count != 0U && !pending.empty()) {
        pending.pop_front();
        --submitted_count;
    }
}

std::vector<pending_io_uring_submission> take_unsubmitted_io_uring_submissions(
    std::deque<pending_io_uring_submission>& pending) {
    std::vector<pending_io_uring_submission> unsubmitted{};
    unsubmitted.reserve(pending.size());
    while (!pending.empty()) {
        unsubmitted.push_back(pending.front());
        pending.pop_front();
    }
    return unsubmitted;
}

bool io_uring_cancel_retry_required(bool cancel_requested,
                                    bool close_requested,
                                    bool cancel_request_submitted,
                                    bool cancel_sqe_in_flight) noexcept {
    return (cancel_requested || close_requested) &&
           !cancel_request_submitted &&
           !cancel_sqe_in_flight;
}

io_uring_completion_result_class io_uring_completion_result_for(
    backend_operation_target target,
    bool cancel_requested,
    bool close_requested,
    bool handle_current,
    int cqe_result) noexcept {
    if (cancel_requested) {
        if (cqe_result == -linux_ecanceled) {
            return io_uring_completion_result_class::cancelled;
        }
        return io_uring_completion_result_class::kernel_result;
    }

    if (target == backend_operation_target::file) {
        if ((close_requested || !handle_current) && cqe_result == -linux_ecanceled) {
            return io_uring_completion_result_class::closed;
        }
        return io_uring_completion_result_class::kernel_result;
    }

    if (close_requested || !handle_current) {
        return io_uring_completion_result_class::closed;
    }
    return io_uring_completion_result_class::kernel_result;
}

bool io_uring_completion_should_report_closed(backend_operation_target target,
                                              bool close_requested,
                                              bool handle_current,
                                              int cqe_result) noexcept {
    return io_uring_completion_result_for(target, false, close_requested,
                                          handle_current, cqe_result) ==
           io_uring_completion_result_class::closed;
}

void attach_io_uring_test_kernel(io_uring_backend& backend,
                                 io_uring_test_kernel& kernel) noexcept {
    backend.test_kernel_ = &kernel;
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
    struct sqe_reservation {
        io_uring_sqe* sqe{};
        unsigned index{};
        unsigned next_tail{};
    };

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

    [[nodiscard]] io_result<sqe_reservation> reserve_sqe() {
        const auto head = std::atomic_ref<unsigned>(*sq_head).load();
        // No SQPOLL is enabled, so a failed enter leaves the unsubmitted tail
        // under userspace control and it can be made invisible to later enters.
        const auto tail = std::atomic_ref<unsigned>(*sq_tail).load();
        if (tail - head >= *sq_ring_entries) {
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "io_uring submission ring is full"));
        }

        const unsigned index = tail & *sq_ring_mask;
        auto* sqe = &sqes[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array[index] = index;
        return sqe_reservation{.sqe = sqe, .index = index, .next_tail = tail + 1U};
    }

    void publish_sqe(const sqe_reservation& reservation) noexcept {
        (void)reservation.index;
        std::atomic_ref<unsigned>(*sq_tail).store(reservation.next_tail);
        ++pending_submissions;
    }

    [[nodiscard]] io_result<unsigned> submit_pending_once() {
        while (pending_submissions != 0U) {
            const long submitted =
                ::syscall(SYS_io_uring_enter, fd, pending_submissions, 0U, 0U,
                          nullptr, 0U);
            if (submitted > 0) {
                const auto submitted_count = static_cast<unsigned>(submitted);
                pending_submissions =
                    submitted_count >= pending_submissions
                        ? 0U
                        : pending_submissions - submitted_count;
                return submitted_count;
            }
            if (submitted < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(provider_failure(errno));
            }
            return std::unexpected(make_error(vio_error_code::backend_failure,
                                              "io_uring made no submission progress"));
        }
        return 0U;
    }

    void drop_pending_submissions() noexcept {
        if (pending_submissions == 0U) {
            return;
        }
        const auto tail = std::atomic_ref<unsigned>(*sq_tail).load();
        std::atomic_ref<unsigned>(*sq_tail).store(tail - pending_submissions);
        pending_submissions = 0U;
    }

    [[nodiscard]] unsigned pending_submission_count() const noexcept {
        return pending_submissions;
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
    unsigned pending_submissions{0};

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

io_uring_backend::~io_uring_backend() {
    (void)release_registered_resources();
}

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

std::size_t io_uring_backend::registered_buffer_count() const noexcept {
    return registered_buffers_.size();
}

std::size_t io_uring_backend::registered_file_count() const noexcept {
    return registered_files_.size();
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
    if (!is_valid_backend_operation_shape(operation)) {
        return std::unexpected(
            invalid_state_error("io_uring submit received an invalid operation target"));
    }
    if (!supports_operation(capabilities_, operation)) {
        return std::unexpected(opcode_unavailable_error());
    }
    if (operation.id == 0) {
        return std::unexpected(invalid_state_error("operation id must be non-zero"));
    }
    if (!fallback_.is_current_handle(operation.handle)) {
        return std::unexpected(
            invalid_state_error("operation handle token is not current"));
    }
    if (use_kernel_submission()) {
        if (!capabilities_.supports_cancel) {
            return std::unexpected(cancellation_required_error());
        }
        if (auto user_data = encode_operation_user_data(operation.id); !user_data.has_value()) {
            return std::unexpected(user_data.error());
        }
        if (auto valid = validate_kernel_operation(operation); !valid.has_value()) {
            return valid;
        }
    }
    if (active_operation_ids_.contains(operation.id)) {
        return std::unexpected(invalid_state_error("operation id is already active"));
    }
    if (submission_queue_.size() >= options_.submission_queue_capacity) {
        return std::unexpected(submission_queue_full_error());
    }

    active_operation_ids_.insert(operation.id);
    submission_queue_.push_back(io_uring_backend::queued_submission{operation});
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
        completion_queue_.push_back(
            backend_completion{queued->operation.id, cancelled_completion(reason)});
        submission_queue_.erase(queued);
        return {};
    }
    if (use_kernel_submission()) {
        auto found = kernel_operations_.find(operation_id);
        if (found != kernel_operations_.end()) {
            if (!found->second.cancellation.has_value()) {
                found->second.cancellation = reason;
            }
            if (found->second.cancel_request_submitted ||
                found->second.cancel_sqe_in_flight) {
                return {};
            }
            (void)request_kernel_cancellation_for(operation_id, found->second);
            return {};
        }
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
    if (auto released = release_registered_files_for(token); !released.has_value()) {
        return released;
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
    const auto completion_count_before = completion_queue_.size();
    auto flushed = flush_submission_batch();
    if (!flushed.has_value()) {
        return std::unexpected(flushed.error());
    }
    // The count is newly observed completions made drainable by this poll.
    // Submission flushes only publish backend references and are not counted.
    auto observed = observe_completion_batch();
    if (!observed.has_value()) {
        return std::unexpected(observed.error());
    }
    (void)*observed;
    return completion_queue_.size() - completion_count_before;
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

    if (auto released = release_registered_resources(); !released.has_value()) {
        return released;
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
    if (count == 0) {
        return std::unexpected(invalid_state_error(
            "io_uring registered buffer count must be non-zero"));
    }
    if (!capabilities_.available || !capabilities_.supports_registered_buffers) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered buffers unavailable"));
    }
    if (!registered_buffers_.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring buffers are already registered"));
    }
    return std::unexpected(invalid_state_error(
        "io_uring buffer registration requires borrowed buffer views"));
}

void_result io_uring_backend::register_buffers(
    std::span<const io_uring_registered_buffer> buffers) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (buffers.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring registered buffer list must not be empty"));
    }
    if (!capabilities_.available || !capabilities_.supports_registered_buffers) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered buffers unavailable"));
    }
    if (!registered_buffers_.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring buffers are already registered"));
    }
    if (auto count = count_as_u32(buffers.size(),
                                  "io_uring registered buffer count does not fit u32");
        !count.has_value()) {
        return std::unexpected(count.error());
    }
    for (const auto& buffer : buffers) {
        if (buffer.bytes.empty() || buffer.bytes.data() == nullptr) {
            return std::unexpected(invalid_state_error(
                "io_uring registered buffers must reference non-empty storage"));
        }
    }

    if (auto registered = register_buffers_with_provider(buffers);
        !registered.has_value()) {
        return registered;
    }

    registered_buffers_.assign(buffers.begin(), buffers.end());
    return {};
}

void_result io_uring_backend::unregister_buffers() {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (registered_buffers_.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring buffers are not registered"));
    }

    if (auto unregistered = unregister_buffers_with_provider();
        !unregistered.has_value()) {
        return unregistered;
    }
    registered_buffers_.clear();
    return {};
}

void_result io_uring_backend::register_files(std::size_t count) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (count == 0) {
        return std::unexpected(invalid_state_error(
            "io_uring registered file count must be non-zero"));
    }
    if (!capabilities_.available || !capabilities_.supports_registered_files) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered files unavailable"));
    }
    if (!registered_files_.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring files are already registered"));
    }
    return std::unexpected(invalid_state_error(
        "io_uring file registration requires backend handle tokens"));
}

void_result io_uring_backend::register_files(
    std::span<const backend_handle_token> files) {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (files.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring registered file list must not be empty"));
    }
    if (!capabilities_.available || !capabilities_.supports_registered_files) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered files unavailable"));
    }
    if (!registered_files_.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring files are already registered"));
    }
    if (auto count = count_as_u32(files.size(),
                                  "io_uring registered file count does not fit u32");
        !count.has_value()) {
        return std::unexpected(count.error());
    }

    for (std::size_t index = 0; index < files.size(); ++index) {
        if (!fallback_.is_current_handle(files[index])) {
            return std::unexpected(invalid_state_error(
                "io_uring registered file token is not current"));
        }
        if (auto fd = native_handle_as_fd(files[index].native_handle);
            !fd.has_value()) {
            return std::unexpected(fd.error());
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (files[previous] == files[index]) {
                return std::unexpected(invalid_state_error(
                    "io_uring registered file tokens must be unique"));
            }
        }
    }

    if (auto registered = register_files_with_provider(files);
        !registered.has_value()) {
        return registered;
    }

    registered_files_.assign(files.begin(), files.end());
    return {};
}

void_result io_uring_backend::unregister_files() {
    if (closed_) {
        return std::unexpected(closed_error());
    }
    if (registered_files_.empty()) {
        return std::unexpected(invalid_state_error(
            "io_uring files are not registered"));
    }

    if (auto unregistered = unregister_files_with_provider();
        !unregistered.has_value()) {
        return unregistered;
    }
    registered_files_.clear();
    return {};
}

bool io_uring_backend::use_kernel_submission() const noexcept {
    return options_.enable_kernel_submission;
}

bool io_uring_backend::use_test_kernel_submission() const noexcept {
    return test_kernel_ != nullptr;
}

bool io_uring_backend::has_inflight_kernel_work() const noexcept {
    return !kernel_operations_.empty() || !kernel_cancel_operation_ids_.empty();
}

bool io_uring_backend::registered_files_contains(
    backend_handle_token token) const noexcept {
    return std::ranges::find(registered_files_, token) != registered_files_.end();
}

void_result io_uring_backend::ensure_kernel_ring() {
#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (kernel_ring_) {
        return {};
    }

    auto created = kernel_ring::create(options_.submission_queue_capacity);
    if (!created.has_value()) {
        return std::unexpected(created.error());
    }
    kernel_ring_ = std::move(*created);
    return {};
#else
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::validate_kernel_operation(
    const backend_operation& operation) const {
    if (auto fd = native_handle_as_fd(operation.handle.native_handle); !fd.has_value()) {
        return std::unexpected(fd.error());
    }
    if (!is_valid_backend_operation_shape(operation)) {
        return std::unexpected(
            invalid_state_error("io_uring kernel submission received an invalid operation target"));
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
        if (operation.target != backend_operation_target::socket) {
            return std::unexpected(
                invalid_state_error("io_uring accept requires a socket target"));
        }
        return {};
    case backend_operation_kind::connect:
        if (operation.target != backend_operation_target::socket) {
            return std::unexpected(
                invalid_state_error("io_uring connect requires a socket target"));
        }
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
    case backend_operation_kind::fsync:
        if (operation.target != backend_operation_target::file) {
            return std::unexpected(
                invalid_state_error("io_uring fsync requires a file target"));
        }
        return {};
    case backend_operation_kind::close:
    case backend_operation_kind::wake:
        return std::unexpected(
            invalid_state_error("io_uring kernel submission requires an I/O operation"));
    }

    return std::unexpected(
        invalid_state_error("io_uring kernel submission requires a known operation"));
}

void_result io_uring_backend::register_buffers_with_provider(
    std::span<const io_uring_registered_buffer> buffers) {
    if (!use_kernel_submission()) {
        return {};
    }
    if (use_test_kernel_submission()) {
        ++test_kernel_->register_buffers_calls;
        return {};
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter) && \
    defined(SYS_io_uring_register)
    if (auto ensured = ensure_kernel_ring(); !ensured.has_value()) {
        return ensured;
    }

    std::vector<iovec> iovecs{};
    iovecs.reserve(buffers.size());
    for (const auto& buffer : buffers) {
        iovecs.push_back(iovec{
            .iov_base = buffer.bytes.data(),
            .iov_len = buffer.bytes.size(),
        });
    }

    const auto count =
        count_as_u32(iovecs.size(),
                     "io_uring registered buffer count does not fit u32");
    if (!count.has_value()) {
        return std::unexpected(count.error());
    }

    const long result =
        ::syscall(SYS_io_uring_register, kernel_ring_->fd,
                  IORING_REGISTER_BUFFERS, iovecs.data(), *count);
    if (result < 0) {
        return std::unexpected(provider_failure(errno));
    }
    return {};
#else
    (void)buffers;
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::unregister_buffers_with_provider() {
    if (!use_kernel_submission()) {
        return {};
    }
    if (use_test_kernel_submission()) {
        ++test_kernel_->unregister_buffers_calls;
        return {};
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter) && \
    defined(SYS_io_uring_register)
    if (!kernel_ring_) {
        return std::unexpected(
            invalid_state_error("io_uring buffer unregister requires an active ring"));
    }
    const long result =
        ::syscall(SYS_io_uring_register, kernel_ring_->fd,
                  IORING_UNREGISTER_BUFFERS, nullptr, 0U);
    if (result < 0) {
        return std::unexpected(provider_failure(errno));
    }
    return {};
#else
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::register_files_with_provider(
    std::span<const backend_handle_token> files) {
    if (!use_kernel_submission()) {
        return {};
    }
    if (use_test_kernel_submission()) {
        ++test_kernel_->register_files_calls;
        return {};
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter) && \
    defined(SYS_io_uring_register)
    if (auto ensured = ensure_kernel_ring(); !ensured.has_value()) {
        return ensured;
    }

    std::vector<int> descriptors{};
    descriptors.reserve(files.size());
    for (const auto file : files) {
        auto fd = native_handle_as_fd(file.native_handle);
        if (!fd.has_value()) {
            return std::unexpected(fd.error());
        }
        descriptors.push_back(*fd);
    }

    const auto count =
        count_as_u32(descriptors.size(),
                     "io_uring registered file count does not fit u32");
    if (!count.has_value()) {
        return std::unexpected(count.error());
    }

    const long result =
        ::syscall(SYS_io_uring_register, kernel_ring_->fd,
                  IORING_REGISTER_FILES, descriptors.data(), *count);
    if (result < 0) {
        return std::unexpected(provider_failure(errno));
    }
    return {};
#else
    (void)files;
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::unregister_files_with_provider() {
    if (!use_kernel_submission()) {
        return {};
    }
    if (use_test_kernel_submission()) {
        ++test_kernel_->unregister_files_calls;
        return {};
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter) && \
    defined(SYS_io_uring_register)
    if (!kernel_ring_) {
        return std::unexpected(
            invalid_state_error("io_uring file unregister requires an active ring"));
    }
    const long result =
        ::syscall(SYS_io_uring_register, kernel_ring_->fd,
                  IORING_UNREGISTER_FILES, nullptr, 0U);
    if (result < 0) {
        return std::unexpected(provider_failure(errno));
    }
    return {};
#else
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::release_registered_resources() {
    if (!registered_buffers_.empty()) {
        if (auto unregistered = unregister_buffers_with_provider();
            !unregistered.has_value()) {
            return unregistered;
        }
        registered_buffers_.clear();
    }

    if (!registered_files_.empty()) {
        if (auto unregistered = unregister_files_with_provider();
            !unregistered.has_value()) {
            return unregistered;
        }
        registered_files_.clear();
    }

    return {};
}

void_result io_uring_backend::release_registered_files_for(
    backend_handle_token token) {
    if (!registered_files_contains(token)) {
        return {};
    }
    if (auto unregistered = unregister_files_with_provider();
        !unregistered.has_value()) {
        return unregistered;
    }
    registered_files_.clear();
    return {};
}

void_result io_uring_backend::submit_to_fallback(queued_submission queued) {
    auto submitted = fallback_.submit(queued.operation);
    if (!submitted.has_value()) {
        active_operation_ids_.erase(queued.operation.id);
        return submitted;
    }
    return {};
}

void_result io_uring_backend::submit_to_kernel(const queued_submission& queued) {
    const backend_operation& operation = queued.operation;
    if (auto valid = validate_kernel_operation(operation); !valid.has_value()) {
        return valid;
    }
    if (use_test_kernel_submission()) {
        return submit_to_test_kernel(operation);
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (auto ensured = ensure_kernel_ring(); !ensured.has_value()) {
        return ensured;
    }

    const auto fd = native_handle_as_fd(operation.handle.native_handle);
    if (!fd.has_value()) {
        return std::unexpected(fd.error());
    }
    const auto user_data = encode_operation_user_data(operation.id);
    if (!user_data.has_value()) {
        return std::unexpected(user_data.error());
    }

    std::uint8_t opcode{};
    std::uint64_t offset{};
    std::uint64_t address{};
    unsigned length{};
    unsigned accept_flags{};

    switch (operation.kind) {
    case backend_operation_kind::read: {
        const auto size = span_size_as_u32(operation.read_buffer.size());
        if (!size.has_value()) {
            return std::unexpected(size.error());
        }
        opcode = IORING_OP_READ;
        offset = operation.target == backend_operation_target::file ? operation.offset : 0U;
        address = reinterpret_cast<std::uintptr_t>(operation.read_buffer.data());
        length = *size;
        break;
    }
    case backend_operation_kind::write: {
        const auto size = span_size_as_u32(operation.write_buffer.size());
        if (!size.has_value()) {
            return std::unexpected(size.error());
        }
        opcode = IORING_OP_WRITE;
        offset = operation.target == backend_operation_target::file ? operation.offset : 0U;
        address = reinterpret_cast<std::uintptr_t>(operation.write_buffer.data());
        length = *size;
        break;
    }
    case backend_operation_kind::accept:
        opcode = IORING_OP_ACCEPT;
        accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        break;
    case backend_operation_kind::connect:
        opcode = IORING_OP_CONNECT;
        offset = operation.socket_address.size();
        address = reinterpret_cast<std::uintptr_t>(operation.socket_address.data());
        break;
    case backend_operation_kind::fsync:
        opcode = IORING_OP_FSYNC;
        break;
    case backend_operation_kind::close:
    case backend_operation_kind::wake:
        return std::unexpected(
            invalid_state_error("io_uring kernel submission requires an I/O operation"));
    }

    auto reservation = kernel_ring_->reserve_sqe();
    if (!reservation.has_value()) {
        return std::unexpected(reservation.error());
    }

    auto* sqe = reservation->sqe;
    sqe->opcode = opcode;
    sqe->fd = *fd;
    sqe->off = offset;
    sqe->addr = address;
    sqe->len = length;
    sqe->accept_flags = accept_flags;
    sqe->user_data = *user_data;

    kernel_operations_.emplace(operation.id,
                               io_uring_backend::kernel_operation{.operation = operation});
    pending_kernel_submissions_.push_back(
        detail::pending_io_uring_submission{
            .kind = detail::pending_io_uring_submission_kind::operation,
            .operation_id = operation.id,
        });
    kernel_ring_->publish_sqe(*reservation);
    if (auto submitted = progress_kernel_submissions(); !submitted.has_value()) {
        return std::unexpected(submitted.error());
    }
    return {};
#else
    (void)operation;
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::submit_to_test_kernel(
    const backend_operation& operation) {
    test_kernel_->submissions.push_back(
        detail::io_uring_test_submission{
            .operation_id = operation.id,
            .used_registered_buffer = false,
            .used_registered_file = false,
        });
    kernel_operations_.emplace(operation.id,
                               io_uring_backend::kernel_operation{.operation = operation});
    return {};
}

void_result io_uring_backend::request_kernel_cancellations_for(
    backend_handle_token token) {
    if (kernel_operations_.empty()) {
        return {};
    }
    if (!capabilities_.supports_cancel) {
        return std::unexpected(cancellation_required_error());
    }

    std::vector<std::size_t> operations_to_cancel{};
    operations_to_cancel.reserve(kernel_operations_.size());
    for (auto& [operation_id, operation] : kernel_operations_) {
        if (token.generation != 0 && operation.operation.handle != token) {
            continue;
        }

        operation.close_requested = true;
        if (!operation.cancel_request_submitted && !operation.cancel_sqe_in_flight) {
            operations_to_cancel.push_back(operation_id);
        }
    }

    for (const auto operation_id : operations_to_cancel) {
        auto found = kernel_operations_.find(operation_id);
        if (found == kernel_operations_.end() ||
            found->second.cancel_request_submitted ||
            found->second.cancel_sqe_in_flight) {
            continue;
        }
        (void)request_kernel_cancellation_for(operation_id, found->second);
    }

    return {};
}

void_result io_uring_backend::request_kernel_cancellation_for(
    std::size_t operation_id,
    kernel_operation& operation) {
    if (use_test_kernel_submission()) {
        return request_test_kernel_cancellation_for(operation_id, operation);
    }

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

    auto reservation = kernel_ring_->reserve_sqe();
    if (!reservation.has_value()) {
        return std::unexpected(reservation.error());
    }

    auto* sqe = reservation->sqe;
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = -1;
    sqe->addr = *target_user_data;
    sqe->cancel_flags = 0;
    sqe->user_data = *cancel_user_data;

    operation.cancel_request_submitted = true;
    operation.cancel_sqe_in_flight = true;
    kernel_cancel_operation_ids_.insert(operation_id);
    pending_kernel_submissions_.push_back(
        detail::pending_io_uring_submission{
            .kind = detail::pending_io_uring_submission_kind::cancel,
            .operation_id = operation_id,
        });
    kernel_ring_->publish_sqe(*reservation);
    if (auto submitted = progress_kernel_submissions(); !submitted.has_value()) {
        return std::unexpected(submitted.error());
    }

    return {};
#else
    (void)operation_id;
    (void)operation;
    return std::unexpected(unavailable_error());
#endif
}

void_result io_uring_backend::request_test_kernel_cancellation_for(
    std::size_t operation_id,
    kernel_operation& operation) {
    auto& kernel = *test_kernel_;
    if (kernel.cancel_submission_failures != 0U) {
        --kernel.cancel_submission_failures;
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "test io_uring cancel SQE is unavailable"));
    }

    operation.cancel_request_submitted = true;
    operation.cancel_sqe_in_flight = true;
    kernel_cancel_operation_ids_.insert(operation_id);
    kernel.submitted_cancel_operation_ids.push_back(operation_id);
    return {};
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

io_result<std::size_t> io_uring_backend::progress_kernel_submissions() {
#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (!kernel_ring_) {
        return 0U;
    }

    std::size_t visible = 0;
    while (kernel_ring_->pending_submission_count() != 0U) {
        auto submitted = kernel_ring_->submit_pending_once();
        if (submitted.has_value()) {
            if (*submitted == 0U) {
                break;
            }
            detail::discard_submitted_io_uring_submissions(
                pending_kernel_submissions_, *submitted);
            continue;
        }

        const auto failure = submitted.error();
        kernel_ring_->drop_pending_submissions();
        visible += complete_unsubmitted_kernel_submissions(failure);
        break;
    }

    return visible;
#else
    return 0U;
#endif
}

io_result<std::size_t> io_uring_backend::retry_missing_kernel_cancellations() {
    std::size_t visible = 0;
    std::vector<std::size_t> operations_to_retry{};
    operations_to_retry.reserve(kernel_operations_.size());
    for (const auto& [operation_id, operation] : kernel_operations_) {
        // A failed enter can prove an internal cancel SQE was never submitted.
        // Keep the cancel or close/shutdown intent and retry once per poll
        // until either the cancel is submitted or the original operation CQE
        // arrives.
        if (!detail::io_uring_cancel_retry_required(operation.cancellation.has_value(),
                                                    operation.close_requested,
                                                    operation.cancel_request_submitted,
                                                    operation.cancel_sqe_in_flight)) {
            continue;
        }
        operations_to_retry.push_back(operation_id);
    }

    for (const auto operation_id : operations_to_retry) {
        auto found = kernel_operations_.find(operation_id);
        if (found == kernel_operations_.end() ||
            !detail::io_uring_cancel_retry_required(found->second.cancellation.has_value(),
                                                    found->second.close_requested,
                                                    found->second.cancel_request_submitted,
                                                    found->second.cancel_sqe_in_flight)) {
            continue;
        }

        auto retried = request_kernel_cancellation_for(operation_id, found->second);
        if (!retried.has_value()) {
            continue;
        }

        auto progressed = progress_kernel_submissions();
        if (!progressed.has_value()) {
            continue;
        }
        visible += *progressed;
    }
    return visible;
}

std::size_t io_uring_backend::complete_unsubmitted_kernel_submissions(
    const vio_error& error) {
    const auto unsubmitted =
        detail::take_unsubmitted_io_uring_submissions(pending_kernel_submissions_);
    std::size_t visible = 0;

    for (const auto& submission : unsubmitted) {
        if (submission.kind == detail::pending_io_uring_submission_kind::cancel) {
            kernel_cancel_operation_ids_.erase(submission.operation_id);
            if (auto found = kernel_operations_.find(submission.operation_id);
                found != kernel_operations_.end()) {
                found->second.cancel_request_submitted = false;
                found->second.cancel_sqe_in_flight = false;
            }
            continue;
        }

        auto found = kernel_operations_.find(submission.operation_id);
        if (found == kernel_operations_.end()) {
            continue;
        }

        backend_completion completion{};
        completion.operation_id = submission.operation_id;
        if (found->second.cancellation.has_value()) {
            completion.result = cancelled_completion(*found->second.cancellation);
        } else if (closed_ || found->second.close_requested ||
            !fallback_.is_current_handle(found->second.operation.handle)) {
            completion.result = closed_completion();
        } else {
            completion.result = std::unexpected(error);
        }
        kernel_operations_.erase(found);
        completion_queue_.push_back(std::move(completion));
        ++visible;
    }

    return visible;
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
    if (use_test_kernel_submission()) {
        return observe_test_kernel_completions();
    }

#if defined(__linux__) && defined(SYS_io_uring_setup) && defined(SYS_io_uring_enter)
    if (!kernel_ring_) {
        return 0U;
    }

    auto progressed = progress_kernel_submissions();
    if (!progressed.has_value()) {
        return std::unexpected(progressed.error());
    }
    auto retried = retry_missing_kernel_cancellations();
    if (!retried.has_value()) {
        return std::unexpected(retried.error());
    }

    std::vector<io_uring_cqe> batch(options_.completion_batch_limit);
    const std::size_t count = kernel_ring_->drain_cqes(batch);
    std::size_t visible = *progressed + *retried;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& cqe = batch[index];
        const auto decoded = decode_kernel_user_data(cqe.user_data);
        if (decoded.is_cancel) {
            kernel_cancel_operation_ids_.erase(decoded.operation_id);
            if (auto found = kernel_operations_.find(decoded.operation_id);
                found != kernel_operations_.end()) {
                found->second.cancel_sqe_in_flight = false;
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
        const bool handle_current = fallback_.is_current_handle(operation.operation.handle);
        const auto result_class = detail::io_uring_completion_result_for(
            operation.operation.target, operation.cancellation.has_value(),
            operation.close_requested, handle_current, cqe.res);
        if (result_class == detail::io_uring_completion_result_class::cancelled) {
            completion.result = cancelled_completion(*operation.cancellation);
        } else if (result_class == detail::io_uring_completion_result_class::closed) {
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
            case backend_operation_kind::fsync:
                break;
            case backend_operation_kind::close:
            case backend_operation_kind::wake:
                completion.result = std::unexpected(invalid_state_error(
                    "unexpected non-I/O io_uring completion"));
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

io_result<std::size_t> io_uring_backend::observe_test_kernel_completions() {
    auto retried = retry_missing_kernel_cancellations();
    if (!retried.has_value()) {
        return std::unexpected(retried.error());
    }

    auto& kernel = *test_kernel_;
    const std::size_t count =
        std::min(options_.completion_batch_limit, kernel.completions.size());
    std::size_t visible = *retried;

    for (std::size_t index = 0; index < count; ++index) {
        const auto cqe = kernel.completions.front();
        kernel.completions.pop_front();

        if (cqe.kind == detail::io_uring_test_completion_kind::cancel_ack) {
            kernel_cancel_operation_ids_.erase(cqe.operation_id);
            if (auto found = kernel_operations_.find(cqe.operation_id);
                found != kernel_operations_.end()) {
                found->second.cancel_sqe_in_flight = false;
            }
            continue;
        }

        const auto found = kernel_operations_.find(cqe.operation_id);
        if (found == kernel_operations_.end()) {
            continue;
        }

        const auto operation = found->second;
        kernel_operations_.erase(found);

        backend_completion completion{};
        completion.operation_id = cqe.operation_id;
        const bool handle_current = fallback_.is_current_handle(operation.operation.handle);
        const auto result_class = detail::io_uring_completion_result_for(
            operation.operation.target, operation.cancellation.has_value(),
            operation.close_requested, handle_current, cqe.result);
        if (result_class == detail::io_uring_completion_result_class::cancelled) {
            completion.result = cancelled_completion(*operation.cancellation);
        } else if (result_class == detail::io_uring_completion_result_class::closed) {
            completion.result = closed_completion();
        } else if (cqe.result < 0) {
            completion.result = std::unexpected(provider_failure(-cqe.result));
        } else {
            switch (operation.operation.kind) {
            case backend_operation_kind::read:
            case backend_operation_kind::write:
                completion.bytes_transferred = static_cast<std::size_t>(cqe.result);
                break;
            case backend_operation_kind::accept:
                completion.accepted_native_handle = static_cast<std::size_t>(cqe.result);
                break;
            case backend_operation_kind::connect:
                break;
            case backend_operation_kind::fsync:
                break;
            case backend_operation_kind::close:
            case backend_operation_kind::wake:
                completion.result = std::unexpected(invalid_state_error(
                    "unexpected non-I/O test io_uring completion"));
                break;
            }
        }

        completion_queue_.push_back(std::move(completion));
        ++visible;
    }

    return visible;
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
