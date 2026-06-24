#include <voris/io/version.hpp>

namespace voris::io {

std::string_view version() noexcept {
    return library_version;
}

} // namespace voris::io
