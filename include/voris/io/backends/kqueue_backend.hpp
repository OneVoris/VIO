#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

namespace detail {

template <class Event>
using kqueue_udata_t = decltype(std::declval<Event&>().udata);

template <class Event>
[[nodiscard]] auto kqueue_cookie_to_udata(std::uintptr_t cookie) noexcept
    -> kqueue_udata_t<Event> {
    using udata_type = std::remove_cv_t<kqueue_udata_t<Event>>;
    if constexpr (std::is_pointer_v<udata_type>) {
        return reinterpret_cast<kqueue_udata_t<Event>>(cookie);
    } else {
        static_assert(std::is_integral_v<udata_type>,
                      "kqueue udata must be pointer or integral");
        return static_cast<kqueue_udata_t<Event>>(cookie);
    }
}

template <class Event>
[[nodiscard]] std::uintptr_t kqueue_cookie_from_udata(const Event& event) noexcept {
    using udata_type = std::remove_cv_t<kqueue_udata_t<Event>>;
    if constexpr (std::is_pointer_v<udata_type>) {
        return reinterpret_cast<std::uintptr_t>(event.udata);
    } else {
        static_assert(std::is_integral_v<udata_type>,
                      "kqueue udata must be pointer or integral");
        return static_cast<std::uintptr_t>(event.udata);
    }
}

} // namespace detail

class kqueue_backend final : public backend {
public:
    kqueue_backend();
    ~kqueue_backend() override;

    kqueue_backend(const kqueue_backend&) = delete;
    kqueue_backend& operator=(const kqueue_backend&) = delete;

    [[nodiscard]] io_result<backend_handle_token> register_handle(
        std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] void_result close_handle(backend_handle_token token) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] io_result<std::size_t> drain_completions(
        std::span<backend_completion> out) override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

private:
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    void close_owned_descriptor() noexcept;
    [[nodiscard]] void_result initialization_result() const;
#endif

    virtual_backend fallback_;
    int kqueue_fd_{-1};
    bool stopped_{false};
    std::optional<vio_error> initialization_error_{};
};

} // namespace voris::io::backends
