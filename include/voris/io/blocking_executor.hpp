#pragma once

#include <cstddef>
#include <memory>

#include <voris/io/scheduler.hpp>

namespace voris::io {

class blocking_executor {
public:
    blocking_executor(std::size_t worker_count, std::size_t queue_limit);
    explicit blocking_executor(std::size_t queue_limit);
    ~blocking_executor();

    blocking_executor(const blocking_executor&) = delete;
    blocking_executor& operator=(const blocking_executor&) = delete;

    blocking_executor(blocking_executor&&) = delete;
    blocking_executor& operator=(blocking_executor&&) = delete;

    [[nodiscard]] void_result submit(continuation work);

    void shutdown() noexcept;

    [[nodiscard]] bool shutting_down() const;

    [[nodiscard]] std::size_t queued() const;

private:
    struct state;

    std::shared_ptr<state> state_;
};

} // namespace voris::io
