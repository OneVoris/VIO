#include <voris/io/runtime_options.hpp>

namespace voris::io {

void_result runtime_options::validate() const {
    if (shard_count == 0 || queue_limit == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "runtime options require non-zero limits"));
    }
    return {};
}

} // namespace voris::io
