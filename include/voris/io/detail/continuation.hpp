#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace voris::io::detail {

template<class Signature>
class move_only_function;

template<class R, class... Args>
class move_only_function<R(Args...)> {
public:
    move_only_function() noexcept = default;
    move_only_function(std::nullptr_t) noexcept {}

    move_only_function(const move_only_function&) = delete;
    move_only_function& operator=(const move_only_function&) = delete;

    move_only_function(move_only_function&&) noexcept = default;
    move_only_function& operator=(move_only_function&&) noexcept = default;

    template<class Callable>
        requires(!std::same_as<std::remove_cvref_t<Callable>, move_only_function> &&
                 !std::same_as<std::remove_cvref_t<Callable>, std::nullptr_t> &&
                 std::constructible_from<std::decay_t<Callable>, Callable> &&
                 std::is_invocable_r_v<R, std::decay_t<Callable>&, Args...>)
    move_only_function(Callable&& callable) {
        using stored_type = std::decay_t<Callable>;
        if constexpr (nullable_callable<stored_type>) {
            if (callable == nullptr) {
                return;
            }
        }

        target_ = std::make_unique<model<stored_type>>(std::forward<Callable>(callable));
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return target_ != nullptr;
    }

    R operator()(Args... args) {
        if constexpr (std::is_void_v<R>) {
            target_->call(std::forward<Args>(args)...);
        } else {
            return target_->call(std::forward<Args>(args)...);
        }
    }

private:
    struct concept_base {
        concept_base() = default;
        concept_base(const concept_base&) = delete;
        concept_base& operator=(const concept_base&) = delete;
        concept_base(concept_base&&) = delete;
        concept_base& operator=(concept_base&&) = delete;
        virtual ~concept_base() = default;

        virtual R call(Args... args) = 0;
    };

    template<class Callable>
    struct model final : concept_base {
        template<class Source>
            requires std::constructible_from<Callable, Source>
        explicit model(Source&& source)
            : callable(std::forward<Source>(source)) {
        }

        R call(Args... args) override {
            if constexpr (std::is_void_v<R>) {
                std::invoke(callable, std::forward<Args>(args)...);
            } else {
                return std::invoke(callable, std::forward<Args>(args)...);
            }
        }

        Callable callable;
    };

    template<class Callable>
    static constexpr bool nullable_callable =
        std::is_pointer_v<Callable> || std::is_member_pointer_v<Callable>;

    std::unique_ptr<concept_base> target_;
};

} // namespace voris::io::detail

namespace voris::io {

using continuation = detail::move_only_function<void()>;

} // namespace voris::io
