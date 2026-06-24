#pragma once

#include <string_view>

namespace voris::io {

inline constexpr std::string_view library_name = "VIO";
inline constexpr std::string_view library_version = "0.1.0-dev";

[[nodiscard]] std::string_view version() noexcept;

} // namespace voris::io
