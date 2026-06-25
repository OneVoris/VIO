#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <voris/io/runtime_options.hpp>
#include <voris/io/shard.hpp>

namespace voris::io {

class runtime {
public:
    ~runtime();

    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    runtime(runtime&&) noexcept = default;
    runtime& operator=(runtime&&) noexcept = default;

    [[nodiscard]] static io_result<runtime> create(runtime_options options);

    void start();
    void request_stop();
    void join();

    [[nodiscard]] std::size_t shard_count() const noexcept;
    [[nodiscard]] shard& get_shard(std::size_t index);
    [[nodiscard]] std::optional<std::size_t> requested_shard_cpu_affinity(
        std::size_t index) const;
    [[nodiscard]] const runtime_options& options() const noexcept;

private:
    explicit runtime(runtime_options options);

    runtime_options options_;
    std::vector<std::unique_ptr<shard>> shards_;
};

} // namespace voris::io
