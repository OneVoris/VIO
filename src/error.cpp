#include <voris/io/error.hpp>

#include <utility>

namespace voris::io {

vio_error::vio_error(vio_error_code classification_value,
                     std::optional<std::int64_t> provider_code_value,
                     std::string diagnostic_value,
                     std::source_location location_value)
    : classification(classification_value),
      provider_code(provider_code_value),
      diagnostic(std::move(diagnostic_value)),
      location(location_value) {}

std::string_view to_string(vio_error_code code) noexcept {
    switch (code) {
    case vio_error_code::none:
        return "none";
    case vio_error_code::invalid_state:
        return "invalid_state";
    case vio_error_code::cancelled:
        return "cancelled";
    case vio_error_code::deadline_exceeded:
        return "deadline_exceeded";
    case vio_error_code::resource_exhausted:
        return "resource_exhausted";
    case vio_error_code::operation_in_progress:
        return "operation_in_progress";
    case vio_error_code::closed:
        return "closed";
    case vio_error_code::backend_failure:
        return "backend_failure";
    case vio_error_code::unsupported:
        return "unsupported";
    }

    return "unknown";
}

vio_error make_error(vio_error_code classification,
                     std::optional<std::int64_t> provider_code,
                     std::string diagnostic,
                     std::source_location location) {
    return vio_error(classification, provider_code, std::move(diagnostic), location);
}

vio_error make_error(vio_error_code classification,
                     std::int64_t provider_code,
                     std::string diagnostic,
                     std::source_location location) {
    return make_error(classification, std::optional<std::int64_t>{provider_code},
                      std::move(diagnostic), location);
}

vio_error make_error(vio_error_code classification,
                     std::string diagnostic,
                     std::source_location location) {
    return make_error(classification, std::nullopt, std::move(diagnostic), location);
}

} // namespace voris::io
