#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

namespace voris::io {

enum class vio_error_code : std::uint8_t {
    none = 0,
    invalid_state = 1,
    cancelled = 2,
    deadline_exceeded = 3,
    resource_exhausted = 4,
    operation_in_progress = 5,
    closed = 6,
    backend_failure = 7,
    unsupported = 8,
};

struct vio_error {
    vio_error_code classification{vio_error_code::none};
    std::optional<std::int64_t> provider_code{};
    std::string diagnostic{};
    std::source_location location{};

    vio_error() noexcept = default;

    explicit vio_error(
        vio_error_code classification,
        std::optional<std::int64_t> provider_code = std::nullopt,
        std::string diagnostic = {},
        std::source_location location = std::source_location::current());

    [[nodiscard]] friend bool operator==(const vio_error& lhs, const vio_error& rhs) noexcept {
        return lhs.classification == rhs.classification && lhs.provider_code == rhs.provider_code;
    }

    [[nodiscard]] friend std::strong_ordering operator<=>(const vio_error& lhs,
                                                          const vio_error& rhs) noexcept {
        const auto lhs_classification = static_cast<std::uint8_t>(lhs.classification);
        const auto rhs_classification = static_cast<std::uint8_t>(rhs.classification);
        if (const auto order = lhs_classification <=> rhs_classification; order != 0) {
            return order;
        }
        return lhs.provider_code <=> rhs.provider_code;
    }
};

template<class T>
using io_result = std::expected<T, vio_error>;

using void_result = std::expected<void, vio_error>;

[[nodiscard]] std::string_view to_string(vio_error_code code) noexcept;

[[nodiscard]] vio_error make_error(
    vio_error_code classification,
    std::optional<std::int64_t> provider_code = std::nullopt,
    std::string diagnostic = {},
    std::source_location location = std::source_location::current());

[[nodiscard]] vio_error make_error(vio_error_code classification,
                                   std::int64_t provider_code,
                                   std::string diagnostic = {},
                                   std::source_location location = std::source_location::current());

[[nodiscard]] vio_error make_error(vio_error_code classification,
                                   std::string diagnostic,
                                   std::source_location location = std::source_location::current());

} // namespace voris::io
