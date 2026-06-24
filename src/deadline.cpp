#include <voris/io/deadline.hpp>

#include <string>

namespace voris::io {

vio_error deadline::cancellation_error(std::source_location location) {
    return make_error(vio_error_code::deadline_exceeded, std::string{"deadline exceeded"}, location);
}

} // namespace voris::io
