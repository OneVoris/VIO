#include <voris/io/backends/iocp_backend.hpp>

#include <algorithm>
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

[[nodiscard]] void_result closed_error() {
    return std::unexpected(make_error(vio_error_code::closed));
}
#else
[[nodiscard]] vio_error unsupported_error() {
    return make_error(vio_error_code::unsupported, "IOCP backend is unavailable");
}
#endif

} // namespace

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

io_result<detail::iocp_completion_key_token> iocp_backend::create_association(
    backend_handle_token token) {
    if (associations_.size() >= detail::iocp_max_association_count) {
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "IOCP association table is full"));
    }

    detail::iocp_completion_key_token key{
        associations_.size() + 1U,
        token.generation,
    };
    if (auto packed = detail::pack_iocp_completion_key(key); !packed.has_value()) {
        return std::unexpected(packed.error());
    }

    associations_.push_back(association_entry{key.association_id, token, true});
    association_by_native_handle_[token.native_handle] = key.association_id;
    return key;
}

void iocp_backend::rollback_association(detail::iocp_completion_key_token key) noexcept {
    if (key.association_id == 0 || key.association_id > associations_.size()) {
        return;
    }

    auto& entry = associations_[key.association_id - 1U];
    entry.open = false;
    if (auto found = association_by_native_handle_.find(entry.token.native_handle);
        found != association_by_native_handle_.end() && found->second == key.association_id) {
        association_by_native_handle_.erase(found);
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

    return detail::iocp_completion_key_token{association_id, token.generation};
}

std::optional<backend_handle_token> iocp_backend::current_handle_for(
    detail::iocp_completion_key_token key) const noexcept {
    if (key.association_id == 0 || key.association_id > associations_.size()) {
        return std::nullopt;
    }

    const auto& entry = associations_[key.association_id - 1U];
    if (!entry.open || entry.token.generation != key.generation ||
        !fallback_.is_current_handle(entry.token)) {
        return std::nullopt;
    }
    return entry.token;
}

io_result<detail::iocp_completion_key_token> detail::iocp_completion_key_for(
    const iocp_backend& backend,
    backend_handle_token token) {
    return backend.completion_key_for(token);
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
    if (native_packets_.size() >= options_.native_packet_capacity) {
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "IOCP native packet queue is full"));
    }

    const auto capacity_remaining = options_.native_packet_capacity - native_packets_.size();
    const auto batch_limit = std::min(options_.completion_batch_limit, capacity_remaining);
    std::vector<OVERLAPPED_ENTRY> entries(batch_limit);
    ULONG removed = 0;
    if (::GetQueuedCompletionStatusEx(static_cast<HANDLE>(completion_port_),
                                      entries.data(),
                                      static_cast<ULONG>(entries.size()),
                                      &removed, 0, FALSE) == 0) {
        const DWORD provider_code = ::GetLastError();
        if (provider_code == WAIT_TIMEOUT) {
            return 0U;
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
        const auto handle = current_handle_for(*completion_key);
        if (!handle.has_value()) {
            ++observed;
            continue;
        }

        // M7-003 will map current OVERLAPPED storage to user completions. M7-002
        // only preserves the native packet and proves stale generation handling.
        native_packets_.push_back(detail::iocp_native_completion_packet{
            static_cast<std::size_t>(entry.dwNumberOfBytesTransferred),
            *completion_key,
            key,
            *handle,
            entry.lpOverlapped,
            static_cast<std::uintptr_t>(entry.Internal),
        });
        ++observed;
    }
    return observed;
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
    return fallback_.submit(operation);
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
    return fallback_.cancel(operation_id, reason);
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
    auto closed = fallback_.close_handle(token);
    if (!closed.has_value()) {
        return closed;
    }
    close_association(token);
    return {};
#else
    (void)token;
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> iocp_backend::poll() {
#if defined(_WIN32)
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }

    auto native_observed = observe_native_completions();
    if (!native_observed.has_value()) {
        return std::unexpected(native_observed.error());
    }
    auto fallback_visible = fallback_.poll();
    if (!fallback_visible.has_value()) {
        return std::unexpected(fallback_visible.error());
    }
    return *native_observed + *fallback_visible;
#else
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> iocp_backend::drain_completions(
    std::span<backend_completion> out) {
#if defined(_WIN32)
    return fallback_.drain_completions(out);
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

    stopped_ = true;
    auto drained = fallback_.shutdown();
    // Shutdown closes the owned IOCP port; native packets that were not mapped
    // before teardown cannot be completed later and are discarded explicitly.
    native_packets_.clear();
    association_by_native_handle_.clear();
    associations_.clear();
    close_owned_port();
    return drained;
#else
    return fallback_.shutdown();
#endif
}

} // namespace voris::io::backends
