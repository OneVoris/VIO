#include <voris/io/error.hpp>

#include <cassert>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>

int main() {
    using namespace voris::io;

    static_assert(std::is_same_v<io_result<int>, std::expected<int, vio_error>>);
    static_assert(std::is_same_v<void_result, std::expected<void, vio_error>>);

    const vio_error first =
        make_error(vio_error_code::backend_failure, 10054, "connection reset by peer");
    const vio_error same_identity =
        make_error(vio_error_code::backend_failure, 10054, "provider text changed");
    const vio_error different_provider =
        make_error(vio_error_code::backend_failure, 32, "broken pipe");
    const vio_error different_classification =
        make_error(vio_error_code::cancelled, 10054, "cancelled by caller");

    assert(first == same_identity);
    assert(!(first != same_identity));
    assert(first != different_provider);
    assert(first != different_classification);
    assert(first.classification == vio_error_code::backend_failure);
    assert(first.provider_code.has_value());
    assert(*first.provider_code == 10054);
    assert(first.diagnostic == "connection reset by peer");
    assert(first.location.file_name() != nullptr);

    const vio_error no_provider = make_error(vio_error_code::unsupported, "not implemented");
    assert(no_provider.classification == vio_error_code::unsupported);
    assert(!no_provider.provider_code.has_value());
    assert(no_provider.diagnostic == "not implemented");

    const io_result<int> value = 42;
    assert(value.has_value());
    assert(*value == 42);

    const io_result<int> failure = std::unexpected(first);
    assert(!failure.has_value());
    assert(failure.error() == same_identity);

    const void_result ok{};
    assert(ok.has_value());

    const void_result cancelled = std::unexpected(make_error(vio_error_code::cancelled));
    assert(!cancelled.has_value());
    assert(cancelled.error().classification == vio_error_code::cancelled);

    assert(to_string(vio_error_code::none) == std::string_view("none"));
    assert(to_string(vio_error_code::invalid_state) == std::string_view("invalid_state"));
    assert(to_string(vio_error_code::cancelled) == std::string_view("cancelled"));
    assert(to_string(vio_error_code::deadline_exceeded) == std::string_view("deadline_exceeded"));
    assert(to_string(vio_error_code::resource_exhausted) == std::string_view("resource_exhausted"));
    assert(to_string(vio_error_code::operation_in_progress) == std::string_view("operation_in_progress"));
    assert(to_string(vio_error_code::closed) == std::string_view("closed"));
    assert(to_string(vio_error_code::backend_failure) == std::string_view("backend_failure"));
    assert(to_string(vio_error_code::unsupported) == std::string_view("unsupported"));

    return 0;
}
