#include <voris/io/backends/iocp_backend.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#endif

namespace voris::io::backends {

namespace {

#if defined(_WIN32)
[[nodiscard]] vio_error provider_failure(DWORD provider_code) {
    return make_error(vio_error_code::backend_failure,
                      static_cast<std::int64_t>(provider_code));
}
#endif

[[nodiscard]] vio_error provider_failure_status(std::uintptr_t provider_code) {
    return make_error(vio_error_code::backend_failure,
                      static_cast<std::int64_t>(provider_code));
}

[[nodiscard]] void_result closed_error() {
    return std::unexpected(make_error(vio_error_code::closed));
}

[[nodiscard]] vio_error invalid_state_error(std::string diagnostic) {
    return make_error(vio_error_code::invalid_state, std::move(diagnostic));
}

[[nodiscard]] void_result closed_completion() {
    return std::unexpected(make_error(vio_error_code::closed));
}

[[nodiscard]] void_result cancelled_completion(cancellation_reason reason) {
    return std::unexpected(make_error(vio_error_code::cancelled,
                                      std::string("IOCP operation cancelled: ") +
                                          std::string(to_string(reason))));
}

[[nodiscard]] bool iocp_status_is_cancelled(std::uintptr_t status) noexcept {
    return status == detail::iocp_status_operation_aborted ||
           status == detail::iocp_status_cancelled;
}

#if !defined(_WIN32)
[[nodiscard]] vio_error unsupported_error() {
    return make_error(vio_error_code::unsupported, "IOCP backend is unavailable");
}
#endif

} // namespace

struct iocp_backend::operation_storage {
    backend_operation operation{};
    detail::iocp_completion_key_token completion_key{};
    std::optional<cancellation_reason> cancellation{};
    bool cancel_requested{};
    bool close_requested{};
    bool native_submitted{};

#if defined(_WIN32)
    OVERLAPPED overlapped{};

    [[nodiscard]] void* overlapped_address() noexcept {
        return &overlapped;
    }
#else
    std::byte overlapped{};

    [[nodiscard]] void* overlapped_address() noexcept {
        return &overlapped;
    }
#endif
};

void iocp_backend::operation_storage_deleter::operator()(
    operation_storage* storage) const noexcept {
    delete storage;
}

iocp_backend::iocp_backend(iocp_backend_options options) : options_(options) {
    if (options_.completion_batch_limit == 0) {
        options_.completion_batch_limit = 1;
    }
    options_.completion_batch_limit =
        std::min(options_.completion_batch_limit,
                 detail::iocp_max_completion_batch_limit);
    if (options_.native_packet_capacity == 0) {
        options_.native_packet_capacity = options_.completion_batch_limit;
    }
    options_.native_packet_capacity =
        std::min(options_.native_packet_capacity,
                 detail::iocp_max_native_packet_capacity);
    if (options_.association_capacity == 0) {
        options_.association_capacity = 1;
    }
    options_.association_capacity =
        std::min(options_.association_capacity,
                 detail::iocp_max_association_count);

#if defined(_WIN32)
    completion_port_ =
        ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (completion_port_ == nullptr) {
        initialization_error_ = provider_failure(::GetLastError());
    }
#endif
}

iocp_backend::~iocp_backend() {
#if defined(_WIN32)
    close_owned_port();
#endif
}

std::size_t iocp_backend::completion_batch_limit() const noexcept {
    return options_.completion_batch_limit;
}

std::size_t iocp_backend::native_packet_capacity() const noexcept {
    return options_.native_packet_capacity;
}

std::size_t iocp_backend::association_capacity() const noexcept {
    return options_.association_capacity;
}

io_result<detail::iocp_completion_key_token> iocp_backend::create_association(
    backend_handle_token token) {
    while (!free_association_ids_.empty()) {
        const auto association_id = free_association_ids_.back();
        free_association_ids_.pop_back();
        if (association_id == 0 || association_id > associations_.size()) {
            continue;
        }

        auto& entry = associations_[association_id - 1U];
        if (entry.open || !entry.reusable) {
            continue;
        }

        if (entry.bump_generation_on_reuse) {
            if (entry.generation >= detail::iocp_completion_key_low_mask) {
                entry.reusable = false;
                entry.bump_generation_on_reuse = false;
                continue;
            }
            ++entry.generation;
        } else if (entry.generation == 0) {
            entry.generation = 1U;
        }

        detail::iocp_completion_key_token key{association_id, entry.generation};
        if (auto packed = detail::pack_iocp_completion_key(key); !packed.has_value()) {
            entry.reusable = false;
            entry.bump_generation_on_reuse = false;
            return std::unexpected(packed.error());
        }

        entry.token = token;
        entry.open = true;
        entry.reusable = false;
        entry.bump_generation_on_reuse = false;
        association_by_native_handle_[token.native_handle] = association_id;
        return key;
    }

    if (associations_.size() >= options_.association_capacity) {
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "IOCP association table is full"));
    }

    detail::iocp_completion_key_token key{
        associations_.size() + 1U,
        1U,
    };
    if (auto packed = detail::pack_iocp_completion_key(key); !packed.has_value()) {
        return std::unexpected(packed.error());
    }

    associations_.push_back(association_entry{
        key.association_id,
        token,
        key.generation,
        true,
        false,
        false,
    });
    association_by_native_handle_[token.native_handle] = key.association_id;
    return key;
}

void iocp_backend::rollback_association(detail::iocp_completion_key_token key) noexcept {
    if (key.association_id == 0 || key.association_id > associations_.size()) {
        return;
    }

    auto& entry = associations_[key.association_id - 1U];
    if (entry.generation != key.generation) {
        return;
    }
    entry.open = false;
    if (auto found = association_by_native_handle_.find(entry.token.native_handle);
        found != association_by_native_handle_.end() && found->second == key.association_id) {
        association_by_native_handle_.erase(found);
    }
    entry.token = {};
    entry.bump_generation_on_reuse = false;
    if (!entry.reusable) {
        entry.reusable = true;
        free_association_ids_.push_back(key.association_id);
    }
}

void iocp_backend::close_association(backend_handle_token token) noexcept {
    auto found = association_by_native_handle_.find(token.native_handle);
    if (found == association_by_native_handle_.end()) {
        return;
    }

    const auto association_id = found->second;
    if (association_id == 0 || association_id > associations_.size()) {
        association_by_native_handle_.erase(found);
        return;
    }

    auto& entry = associations_[association_id - 1U];
    if (entry.token == token) {
        entry.open = false;
        association_by_native_handle_.erase(found);
        entry.token = {};
        if (entry.generation < detail::iocp_completion_key_low_mask) {
            entry.bump_generation_on_reuse = true;
            if (!entry.reusable) {
                entry.reusable = true;
                free_association_ids_.push_back(association_id);
            }
        } else {
            entry.reusable = false;
            entry.bump_generation_on_reuse = false;
        }
    }
}

io_result<detail::iocp_completion_key_token> iocp_backend::completion_key_for(
    backend_handle_token token) const {
    auto found = association_by_native_handle_.find(token.native_handle);
    if (found == association_by_native_handle_.end()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP handle token has no association"));
    }

    const auto association_id = found->second;
    if (association_id == 0 || association_id > associations_.size()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP association id is not current"));
    }

    const auto& entry = associations_[association_id - 1U];
    if (!entry.open || entry.token != token || !fallback_.is_current_handle(token)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP handle token is not current"));
    }

    return detail::iocp_completion_key_token{association_id, entry.generation};
}

std::optional<backend_handle_token> iocp_backend::current_handle_for(
    detail::iocp_completion_key_token key) const noexcept {
    if (key.association_id == 0 || key.association_id > associations_.size()) {
        return std::nullopt;
    }

    const auto& entry = associations_[key.association_id - 1U];
    if (!entry.open || entry.generation != key.generation ||
        !fallback_.is_current_handle(entry.token)) {
        return std::nullopt;
    }
    return entry.token;
}

void_result iocp_backend::validate_operation_for_submit(
    const backend_operation& operation) const {
    if (operation.id == 0) {
        return std::unexpected(invalid_state_error("operation id must be non-zero"));
    }
    if (!is_valid_backend_operation_shape(operation)) {
        return std::unexpected(
            invalid_state_error("IOCP submit received an invalid operation target"));
    }
    if (!fallback_.is_current_handle(operation.handle)) {
        return std::unexpected(
            invalid_state_error("operation handle token is not current"));
    }
    if (active_operation_ids_.contains(operation.id)) {
        return std::unexpected(invalid_state_error("operation id is already active"));
    }
    return {};
}

void_result iocp_backend::request_cancel_for(operation_storage& storage) {
    if (storage.cancel_requested) {
        return {};
    }
    if (!storage.native_submitted) {
        return {};
    }

#if defined(_WIN32)
    const auto handle = reinterpret_cast<HANDLE>(storage.operation.handle.native_handle);
    if (::CancelIoEx(handle, static_cast<LPOVERLAPPED>(storage.overlapped_address())) == 0) {
        const DWORD provider_code = ::GetLastError();
        if (provider_code != ERROR_NOT_FOUND && provider_code != ERROR_INVALID_HANDLE) {
            return std::unexpected(provider_failure(provider_code));
        }
    }
#endif
    storage.cancel_requested = true;
    ++cancel_request_count_;
    return {};
}

void_result iocp_backend::mark_close_requested(backend_handle_token token) {
    std::vector<std::size_t> operation_ids;
    for (const auto operation_id : operation_submission_order_) {
        const auto found = operations_.find(operation_id);
        if (found != operations_.end() && found->second->operation.handle == token) {
            operation_ids.push_back(operation_id);
        }
    }

    std::optional<vio_error> first_error;
    for (const auto operation_id : operation_ids) {
        auto found = operations_.find(operation_id);
        if (found == operations_.end()) {
            continue;
        }

        auto& storage = *found->second;
        storage.close_requested = true;
        if (!storage.native_submitted) {
            complete_operation_storage(storage, closed_completion());
            continue;
        }

        auto cancelled = request_cancel_for(storage);
        if (!cancelled.has_value() && !first_error.has_value()) {
            first_error = cancelled.error();
        }
    }

    if (first_error.has_value()) {
        return std::unexpected(*first_error);
    }
    return {};
}

void_result iocp_backend::mark_shutdown_requested() {
    std::vector<std::size_t> operation_ids;
    operation_ids.reserve(operation_submission_order_.size());
    for (const auto operation_id : operation_submission_order_) {
        if (operations_.contains(operation_id)) {
            operation_ids.push_back(operation_id);
        }
    }

    std::optional<vio_error> first_error;
    for (const auto operation_id : operation_ids) {
        auto found = operations_.find(operation_id);
        if (found == operations_.end()) {
            continue;
        }

        auto& storage = *found->second;
        storage.close_requested = true;
        if (!storage.native_submitted) {
            complete_operation_storage(storage, closed_completion());
            continue;
        }

        auto cancelled = request_cancel_for(storage);
        if (!cancelled.has_value() && !first_error.has_value()) {
            first_error = cancelled.error();
        }
    }

    if (first_error.has_value()) {
        return std::unexpected(*first_error);
    }
    return {};
}

std::size_t iocp_backend::observe_queued_native_packets() {
    std::size_t observed = 0;
    while (observed < options_.completion_batch_limit && !native_packets_.empty()) {
        const auto packet = native_packets_.front();
        native_packets_.pop_front();
        observe_native_packet(packet);
        ++observed;
    }
    return observed;
}

void iocp_backend::observe_native_packet(
    const detail::iocp_native_completion_packet& packet) {
    if (packet.overlapped == nullptr) {
        return;
    }

    const auto by_overlapped = operation_id_by_overlapped_.find(packet.overlapped);
    if (by_overlapped == operation_id_by_overlapped_.end()) {
        return;
    }

    const auto found = operations_.find(by_overlapped->second);
    if (found == operations_.end()) {
        operation_id_by_overlapped_.erase(by_overlapped);
        return;
    }

    auto& storage = *found->second;
    if (storage.completion_key.association_id != packet.completion_key.association_id ||
        storage.completion_key.generation != packet.completion_key.generation) {
        return;
    }

    complete_native_operation(storage, packet.bytes_transferred, packet.internal_status);
}

void iocp_backend::complete_operation_storage(operation_storage& storage,
                                              void_result result) {
    backend_completion completion{};
    completion.operation_id = storage.operation.id;
    completion.result = std::move(result);

    const auto operation_id = storage.operation.id;
    completion_queue_.push_back(std::move(completion));
    erase_operation_storage(operation_id);
}

void iocp_backend::complete_native_operation(operation_storage& storage,
                                             std::size_t bytes_transferred,
                                             std::uintptr_t internal_status) {
    backend_completion completion{};
    completion.operation_id = storage.operation.id;

    const bool handle_current = fallback_.is_current_handle(storage.operation.handle);
    if (iocp_status_is_cancelled(internal_status) && storage.cancellation.has_value()) {
        completion.result = cancelled_completion(*storage.cancellation);
    } else if (storage.close_requested || stopped_ || !handle_current) {
        completion.result = closed_completion();
    } else if (iocp_status_is_cancelled(internal_status)) {
        completion.result = cancelled_completion(
            storage.cancellation.value_or(cancellation_reason::backend_abort));
    } else if (internal_status != detail::iocp_status_success) {
        completion.result = std::unexpected(provider_failure_status(internal_status));
    } else {
        switch (storage.operation.kind) {
        case backend_operation_kind::read:
        case backend_operation_kind::write:
            completion.bytes_transferred = bytes_transferred;
            break;
        case backend_operation_kind::accept:
            completion.accepted_native_handle = bytes_transferred;
            break;
        case backend_operation_kind::connect:
        case backend_operation_kind::fsync:
            break;
        case backend_operation_kind::close:
        case backend_operation_kind::wake:
            completion.result = std::unexpected(
                invalid_state_error("unexpected non-I/O IOCP completion"));
            break;
        }
    }

    const auto operation_id = storage.operation.id;
    completion_queue_.push_back(std::move(completion));
    erase_operation_storage(operation_id);
}

void iocp_backend::erase_operation_storage(std::size_t operation_id) {
    auto found = operations_.find(operation_id);
    if (found == operations_.end()) {
        return;
    }

    operation_id_by_overlapped_.erase(found->second->overlapped_address());
    operations_.erase(found);
    if (const auto order = operation_order_by_id_.find(operation_id);
        order != operation_order_by_id_.end()) {
        operation_submission_order_.erase(order->second);
        operation_order_by_id_.erase(order);
    }
    maybe_close_stopped_port();
}

void iocp_backend::maybe_close_stopped_port() noexcept {
#if defined(_WIN32)
    if (stopped_ && operations_.empty()) {
        native_packets_.clear();
        association_by_native_handle_.clear();
        associations_.clear();
        free_association_ids_.clear();
        close_owned_port();
    }
#endif
}

io_result<detail::iocp_completion_key_token> detail::iocp_completion_key_for(
    const iocp_backend& backend,
    backend_handle_token token) {
    return backend.completion_key_for(token);
}

void_result detail::queue_iocp_test_packet(iocp_backend& backend,
                                           iocp_completion_key_token key,
                                           std::size_t bytes_transferred,
                                           void* overlapped,
                                           std::uintptr_t internal_status) {
#if defined(_WIN32)
    if (backend.native_packets_.size() >= backend.options_.native_packet_capacity) {
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "IOCP native packet queue is full"));
    }

    const auto raw_key = pack_iocp_completion_key(key);
    if (!raw_key.has_value()) {
        return std::unexpected(raw_key.error());
    }

    backend.native_packets_.push_back(iocp_native_completion_packet{
        bytes_transferred,
        key,
        *raw_key,
        backend.current_handle_for(key).value_or(backend_handle_token{}),
        overlapped,
        internal_status,
    });
    return {};
#else
    (void)backend;
    (void)key;
    (void)bytes_transferred;
    (void)overlapped;
    (void)internal_status;
    return std::unexpected(unsupported_error());
#endif
}

io_result<void*> detail::iocp_overlapped_for(iocp_backend& backend,
                                             std::size_t operation_id) {
#if defined(_WIN32)
    auto found = backend.operations_.find(operation_id);
    if (found == backend.operations_.end()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP operation id has no active storage"));
    }
    return found->second->overlapped_address();
#else
    (void)backend;
    (void)operation_id;
    return std::unexpected(unsupported_error());
#endif
}

void_result detail::mark_iocp_operation_native_submitted(iocp_backend& backend,
                                                         std::size_t operation_id) {
#if defined(_WIN32)
    auto found = backend.operations_.find(operation_id);
    if (found == backend.operations_.end()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "IOCP operation id has no active storage"));
    }
    found->second->native_submitted = true;
    return {};
#else
    (void)backend;
    (void)operation_id;
    return std::unexpected(unsupported_error());
#endif
}

std::size_t detail::iocp_operation_storage_count(const iocp_backend& backend) noexcept {
    return backend.operations_.size();
}

std::size_t detail::iocp_active_operation_id_count(const iocp_backend& backend) noexcept {
    return backend.active_operation_ids_.size();
}

std::size_t detail::iocp_cancel_request_count(const iocp_backend& backend) noexcept {
    return backend.cancel_request_count_;
}

std::size_t detail::iocp_native_packet_count(const iocp_backend& backend) noexcept {
    return backend.native_packets_.size();
}

io_result<std::size_t> detail::drain_iocp_native_packets(
    iocp_backend& backend,
    std::span<iocp_native_completion_packet> out) {
    if (out.empty()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "native packet drain output must not be empty"));
    }

    const auto count = std::min(out.size(), backend.native_packets_.size());
    for (std::size_t index = 0; index < count; ++index) {
        out[index] = backend.native_packets_.front();
        backend.native_packets_.pop_front();
    }
    return count;
}

#if defined(_WIN32)
void iocp_backend::close_owned_port() noexcept {
    if (completion_port_ != nullptr) {
        (void)::CloseHandle(static_cast<HANDLE>(completion_port_));
        completion_port_ = nullptr;
    }
}

void_result iocp_backend::initialization_result() const {
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }
    return {};
}

void_result detail::post_iocp_test_packet(iocp_backend& backend,
                                          iocp_completion_key_token key,
                                          std::size_t bytes_transferred,
                                          void* overlapped) {
    if (backend.stopped_) {
        return closed_error();
    }
    if (auto initialized = backend.initialization_result(); !initialized.has_value()) {
        return initialized;
    }

    auto raw_key = pack_iocp_completion_key(key);
    if (!raw_key.has_value()) {
        return std::unexpected(raw_key.error());
    }

    if (::PostQueuedCompletionStatus(static_cast<HANDLE>(backend.completion_port_),
                                     static_cast<DWORD>(bytes_transferred),
                                     static_cast<ULONG_PTR>(*raw_key),
                                     static_cast<LPOVERLAPPED>(overlapped)) == 0) {
        return std::unexpected(provider_failure(::GetLastError()));
    }
    return {};
}

io_result<std::size_t> iocp_backend::observe_native_completions() {
    if (completion_port_ == nullptr) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    const std::size_t queued_observed = observe_queued_native_packets();
    if (queued_observed >= options_.completion_batch_limit) {
        return queued_observed;
    }
    if (completion_port_ == nullptr) {
        return queued_observed;
    }

    const auto batch_limit = options_.completion_batch_limit - queued_observed;
    std::vector<OVERLAPPED_ENTRY> entries(batch_limit);
    ULONG removed = 0;
    if (::GetQueuedCompletionStatusEx(static_cast<HANDLE>(completion_port_),
                                      entries.data(),
                                      static_cast<ULONG>(entries.size()),
                                      &removed, 0, FALSE) == 0) {
        const DWORD provider_code = ::GetLastError();
        if (provider_code == WAIT_TIMEOUT) {
            return queued_observed;
        }
        return std::unexpected(provider_failure(provider_code));
    }

    std::size_t observed = 0;
    for (ULONG index = 0; index < removed; ++index) {
        const auto& entry = entries[static_cast<std::size_t>(index)];
        const auto key = static_cast<std::uintptr_t>(entry.lpCompletionKey);
        if (detail::is_iocp_wake_completion_key(key) &&
            entry.lpOverlapped == nullptr) {
            ++observed;
            continue;
        }

        const auto completion_key = detail::unpack_iocp_completion_key(key);
        if (!completion_key.has_value()) {
            ++observed;
            continue;
        }

        observe_native_packet(detail::iocp_native_completion_packet{
            static_cast<std::size_t>(entry.dwNumberOfBytesTransferred),
            *completion_key,
            key,
            current_handle_for(*completion_key).value_or(backend_handle_token{}),
            entry.lpOverlapped,
            static_cast<std::uintptr_t>(entry.Internal),
        });
        ++observed;
    }
    return queued_observed + observed;
}
#else
void_result detail::post_iocp_test_packet(iocp_backend&,
                                          iocp_completion_key_token,
                                          std::size_t,
                                          void*) {
    return std::unexpected(unsupported_error());
}
#endif

io_result<backend_handle_token> iocp_backend::register_handle(std::size_t native_handle) {
#if defined(_WIN32)
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return std::unexpected(initialized.error());
    }

    auto token = fallback_.register_handle(native_handle);
    if (!token.has_value()) {
        return token;
    }

    auto association_key = create_association(*token);
    if (!association_key.has_value()) {
        (void)fallback_.close_handle(*token);
        return std::unexpected(association_key.error());
    }

    auto completion_key = detail::pack_iocp_completion_key(*association_key);
    if (!completion_key.has_value()) {
        rollback_association(*association_key);
        (void)fallback_.close_handle(*token);
        return std::unexpected(completion_key.error());
    }

    const auto associated = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(native_handle),
        static_cast<HANDLE>(completion_port_),
        static_cast<ULONG_PTR>(*completion_key), 0);
    if (associated == nullptr) {
        const DWORD provider_code = ::GetLastError();
        rollback_association(*association_key);
        (void)fallback_.close_handle(*token);
        return std::unexpected(provider_failure(provider_code));
    }

    return token;
#else
    (void)native_handle;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::submit(backend_operation operation) {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    if (auto valid = validate_operation_for_submit(operation); !valid.has_value()) {
        return valid;
    }
    auto key = completion_key_for(operation.handle);
    if (!key.has_value()) {
        return std::unexpected(key.error());
    }

    operation_storage_ptr storage{new operation_storage()};
    storage->operation = operation;
    storage->completion_key = *key;
    void* overlapped = storage->overlapped_address();

    active_operation_ids_.insert(operation.id);
    operation_id_by_overlapped_[overlapped] = operation.id;
    operations_.emplace(operation.id, std::move(storage));
    operation_submission_order_.push_back(operation.id);
    operation_order_by_id_[operation.id] = std::prev(operation_submission_order_.end());
    return {};
#else
    (void)operation;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    if (operation_id == 0) {
        return std::unexpected(invalid_state_error("operation id must be non-zero"));
    }

    auto found = operations_.find(operation_id);
    if (found == operations_.end()) {
        return std::unexpected(invalid_state_error("operation id is not active"));
    }

    auto& storage = *found->second;
    if (storage.close_requested) {
        return closed_error();
    }
    if (!storage.cancellation.has_value()) {
        storage.cancellation = reason;
    }
    const auto first_reason = *storage.cancellation;
    if (!storage.native_submitted) {
        complete_operation_storage(storage, cancelled_completion(first_reason));
        return {};
    }
    return request_cancel_for(storage);
#else
    (void)operation_id;
    (void)reason;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::close_handle(backend_handle_token token) {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    if (!fallback_.is_current_handle(token)) {
        return std::unexpected(invalid_state_error("backend handle token is not current"));
    }
    auto marked = mark_close_requested(token);
    auto closed = fallback_.close_handle(token);
    if (!closed.has_value()) {
        return closed;
    }
    close_association(token);
    return marked;
#else
    (void)token;
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> iocp_backend::poll() {
#if defined(_WIN32)
    if (!completion_queue_.empty()) {
        return completion_queue_.size();
    }
    if (stopped_ && operations_.empty() && completion_port_ == nullptr) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }

    auto native_observed = observe_native_completions();
    if (!native_observed.has_value()) {
        return std::unexpected(native_observed.error());
    }
    return *native_observed;
#else
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> iocp_backend::drain_completions(
    std::span<backend_completion> out) {
#if defined(_WIN32)
    if (out.empty()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "completion drain output must not be empty"));
    }

    const auto count = std::min(out.size(), completion_queue_.size());
    for (std::size_t index = 0; index < count; ++index) {
        active_operation_ids_.erase(completion_queue_.front().operation_id);
        out[index] = std::move(completion_queue_.front());
        completion_queue_.pop_front();
    }
    return count;
#else
    (void)out;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::wake() {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }

    if (::PostQueuedCompletionStatus(static_cast<HANDLE>(completion_port_), 0,
                                     static_cast<ULONG_PTR>(
                                         detail::iocp_wake_completion_key),
                                     nullptr) == 0) {
        return std::unexpected(provider_failure(::GetLastError()));
    }
    return {};
#else
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::shutdown() {
#if defined(_WIN32)
    if (stopped_) {
        return {};
    }

    auto marked = mark_shutdown_requested();
    stopped_ = true;
    auto drained = fallback_.shutdown();
    maybe_close_stopped_port();
    if (!marked.has_value()) {
        return marked;
    }
    return drained;
#else
    return fallback_.shutdown();
#endif
}

} // namespace voris::io::backends
