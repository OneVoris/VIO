#include <voris/io/backends/kqueue_backend.hpp>

#include <array>

#include "test_assert.hpp"

namespace {

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = voris::io::backend_operation_kind::read;
    result.handle = token;
    return result;
}

} // namespace

int main() {
    using namespace voris::io;

    backends::kqueue_backend backend;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    auto token = backend.register_handle(1);
    assert(token.has_value());
    assert(backend.submit(operation(1, *token)).has_value());
    assert(backend.close_handle(*token).has_value());
    std::array<backend_completion, 1> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
#else
    auto result = backend.register_handle(1);
    assert(!result.has_value());
    assert(result.error().classification == vio_error_code::unsupported);
#endif
    assert(backend.shutdown().has_value());
    return 0;
}
