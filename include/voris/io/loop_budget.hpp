#pragma once

#include <cstddef>
#include <expected>

#include <voris/io/error.hpp>

namespace voris::io {

struct loop_budget {
    std::size_t task_budget{64};
    std::size_t completion_budget{64};
    std::size_t timer_budget{64};

    [[nodiscard]] void_result validate() const {
        if (task_budget == 0 || completion_budget == 0 || timer_budget == 0) {
            return std::unexpected(
                make_error(vio_error_code::invalid_state,
                           "loop budgets require non-zero task, completion, and timer limits"));
        }
        return {};
    }
};

class loop_budget_slice {
public:
    [[nodiscard]] static io_result<loop_budget_slice> create(loop_budget budget) {
        auto valid = budget.validate();
        if (!valid.has_value()) {
            return std::unexpected(valid.error());
        }
        return loop_budget_slice(budget);
    }

    [[nodiscard]] bool consume_task() noexcept {
        return consume(budget_.task_budget, consumed_tasks_);
    }

    [[nodiscard]] bool consume_completion() noexcept {
        return consume(budget_.completion_budget, consumed_completions_);
    }

    [[nodiscard]] bool consume_timer() noexcept {
        return consume(budget_.timer_budget, consumed_timers_);
    }

    [[nodiscard]] std::size_t remaining_tasks() const noexcept {
        return remaining(budget_.task_budget, consumed_tasks_);
    }

    [[nodiscard]] std::size_t remaining_completions() const noexcept {
        return remaining(budget_.completion_budget, consumed_completions_);
    }

    [[nodiscard]] std::size_t remaining_timers() const noexcept {
        return remaining(budget_.timer_budget, consumed_timers_);
    }

    [[nodiscard]] std::size_t consumed_tasks() const noexcept {
        return consumed_tasks_;
    }

    [[nodiscard]] std::size_t consumed_completions() const noexcept {
        return consumed_completions_;
    }

    [[nodiscard]] std::size_t consumed_timers() const noexcept {
        return consumed_timers_;
    }

private:
    explicit loop_budget_slice(loop_budget budget) noexcept
        : budget_(budget) {}

    [[nodiscard]] static bool consume(std::size_t limit, std::size_t& consumed) noexcept {
        if (consumed >= limit) {
            return false;
        }
        ++consumed;
        return true;
    }

    [[nodiscard]] static std::size_t remaining(std::size_t limit, std::size_t consumed) noexcept {
        return consumed >= limit ? 0 : limit - consumed;
    }

    loop_budget budget_;
    std::size_t consumed_tasks_{0};
    std::size_t consumed_completions_{0};
    std::size_t consumed_timers_{0};
};

} // namespace voris::io
