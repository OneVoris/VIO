#include <voris/io/detail/native_handle_registry.hpp>

namespace voris::io::detail {

native_handle_token native_handle_registry::register_handle(std::size_t native_handle) {
    auto& entry = entries_[native_handle];
    ++entry.generation;
    entry.open = true;
    return native_handle_token{native_handle, entry.generation};
}

void_result native_handle_registry::close(native_handle_token token) {
    auto found = entries_.find(token.id);
    if (found == entries_.end() || found->second.generation != token.generation ||
        !found->second.open) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "stale native handle token"));
    }
    found->second.open = false;
    return {};
}

bool native_handle_registry::is_current(native_handle_token token) const noexcept {
    auto found = entries_.find(token.id);
    return found != entries_.end() && found->second.open &&
           found->second.generation == token.generation;
}

std::size_t native_handle_registry::generation(std::size_t native_handle) const noexcept {
    auto found = entries_.find(native_handle);
    if (found == entries_.end()) {
        return 0;
    }
    return found->second.generation;
}

} // namespace voris::io::detail
