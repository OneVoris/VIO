#include <voris/io/backends/io_uring_backend.hpp>

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

    auto caps = backends::detect_io_uring_capabilities();
    backends::io_uring_backend backend(caps);
    assert(backend.capabilities().available == caps.available);

#if defined(__linux__)
    assert(backend.default_eligible());
    auto token = backend.register_handle(1);
    assert(token.has_value());
    assert(backend.submit(operation(1, *token)).has_value());
    assert(backend.cancel(1, cancellation_reason::manual).has_value());
    assert(backend.poll().has_value());
    assert(backend.close_handle(*token).has_value());
    std::array<backend_completion, 2> completions{};
    assert(backend.drain_completions(completions).has_value());
    assert(backend.register_buffers(2).has_value());
    assert(backend.register_files(2).has_value());
#else
    assert(!backend.default_eligible());
    auto unsupported = backend.register_handle(1);
    assert(!unsupported.has_value());
    assert(unsupported.error().classification == vio_error_code::unsupported);
    assert(!backend.register_buffers(2).has_value());
    assert(!backend.register_files(2).has_value());
#endif

    assert(backend.shutdown().has_value());
    return 0;
}
