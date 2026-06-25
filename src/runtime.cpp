#include <voris/io/runtime.hpp>

namespace voris::io {

runtime::runtime(runtime_options options)
    : options_(options) {
    for (std::size_t i = 0; i != options_.shard_count; ++i) {
        shards_.push_back(std::make_unique<shard>(options_.queue_limit,
                                                  options_.scheduler_budget,
                                                  options_.metrics_config));
    }
}

runtime::~runtime() {
    request_stop();
    join();
}

io_result<runtime> runtime::create(runtime_options options) {
    auto validation = options.validate();
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }
    return runtime(std::move(options));
}

void runtime::start() {
    for (auto& current : shards_) {
        current->start();
    }
}

void runtime::request_stop() {
    for (auto& current : shards_) {
        current->request_stop();
    }
}

void runtime::join() {
    for (auto& current : shards_) {
        current->join();
    }
}

std::size_t runtime::shard_count() const noexcept {
    return shards_.size();
}

shard& runtime::get_shard(std::size_t index) {
    return *shards_.at(index);
}

std::optional<std::size_t> runtime::requested_shard_cpu_affinity(std::size_t index) const {
    (void)shards_.at(index);
    if (!options_.cpu_affinity_start.has_value()) {
        return std::nullopt;
    }
    return *options_.cpu_affinity_start + index;
}

const runtime_options& runtime::options() const noexcept {
    return options_;
}

} // namespace voris::io
