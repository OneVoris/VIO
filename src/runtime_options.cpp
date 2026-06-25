#include <voris/io/runtime_options.hpp>

#include <limits>

namespace voris::io {

void_result runtime_options::validate() const {
    if (shard_count == 0 || queue_limit == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "runtime options require non-zero limits"));
    }
    if (cpu_affinity_start.has_value() &&
        std::numeric_limits<std::size_t>::max() - *cpu_affinity_start < shard_count - 1) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "runtime CPU affinity request range overflows"));
    }
    auto budget_validation = scheduler_budget.validate();
    if (!budget_validation.has_value()) {
        return std::unexpected(budget_validation.error());
    }
    return {};
}

} // namespace voris::io
