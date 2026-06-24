#include <voris/io/cancellation.hpp>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace voris::io {
namespace detail {

struct cancellation_callback_slot {
    explicit cancellation_callback_slot(cancellation_callback callback_value)
        : callback(std::move(callback_value)) {}

    cancellation_callback callback;
    bool registered{true};
};

class cancellation_state {
public:
    std::mutex mutex;
    bool requested{false};
    std::optional<cancellation_reason> reason{};
    std::vector<std::shared_ptr<cancellation_callback_slot>> callbacks;
};

namespace {

thread_local const cancellation_state* tls_locked_cancellation_state = nullptr;

class cancellation_state_lock {
public:
    explicit cancellation_state_lock(cancellation_state& state)
        : lock_(state.mutex),
          previous_(tls_locked_cancellation_state) {
        tls_locked_cancellation_state = &state;
    }

    cancellation_state_lock(const cancellation_state_lock&) = delete;
    cancellation_state_lock& operator=(const cancellation_state_lock&) = delete;

    ~cancellation_state_lock() {
        lock_.unlock();
        tls_locked_cancellation_state = previous_;
    }

private:
    std::unique_lock<std::mutex> lock_;
    const cancellation_state* previous_;
};

} // namespace

bool cancellation_internal_lock_held_for_testing(const cancellation_token& token) noexcept {
    return token.state_ != nullptr && tls_locked_cancellation_state == token.state_.get();
}

} // namespace detail

std::string_view to_string(cancellation_reason reason) noexcept {
    switch (reason) {
    case cancellation_reason::manual:
        return "manual";
    case cancellation_reason::deadline:
        return "deadline";
    case cancellation_reason::scope_shutdown:
        return "scope_shutdown";
    case cancellation_reason::runtime_shutdown:
        return "runtime_shutdown";
    case cancellation_reason::backend_abort:
        return "backend_abort";
    }

    return "unknown";
}

cancellation_registration::cancellation_registration(
    std::shared_ptr<detail::cancellation_state> state,
    std::shared_ptr<detail::cancellation_callback_slot> slot) noexcept
    : state_(std::move(state)),
      slot_(std::move(slot)) {}

cancellation_registration::~cancellation_registration() {
    unregister();
}

cancellation_registration::cancellation_registration(cancellation_registration&& other) noexcept
    : state_(std::move(other.state_)),
      slot_(std::move(other.slot_)) {}

cancellation_registration& cancellation_registration::operator=(
    cancellation_registration&& other) noexcept {
    if (this != &other) {
        unregister();
        state_ = std::move(other.state_);
        slot_ = std::move(other.slot_);
    }
    return *this;
}

void cancellation_registration::unregister() noexcept {
    auto state = std::move(state_);
    auto slot = std::move(slot_);
    if (!state || !slot) {
        return;
    }

    detail::cancellation_state_lock guard(*state);
    if (!slot->registered) {
        return;
    }

    slot->registered = false;
    auto& callbacks = state->callbacks;
    callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), slot), callbacks.end());
}

bool cancellation_registration::active() const {
    if (!state_ || !slot_) {
        return false;
    }

    detail::cancellation_state_lock guard(*state_);
    return slot_->registered;
}

cancellation_token::cancellation_token(std::shared_ptr<detail::cancellation_state> state) noexcept
    : state_(std::move(state)) {}

bool cancellation_token::can_be_cancelled() const noexcept {
    return state_ != nullptr;
}

bool cancellation_token::cancellation_requested() const {
    if (!state_) {
        return false;
    }

    detail::cancellation_state_lock guard(*state_);
    return state_->requested;
}

std::optional<cancellation_reason> cancellation_token::reason() const {
    if (!state_) {
        return std::nullopt;
    }

    detail::cancellation_state_lock guard(*state_);
    return state_->reason;
}

cancellation_registration cancellation_token::register_callback(
    cancellation_callback callback) const {
    if (!state_ || !callback) {
        return {};
    }

    auto slot = std::make_shared<detail::cancellation_callback_slot>(std::move(callback));
    std::optional<cancellation_reason> immediate_reason;

    {
        detail::cancellation_state_lock guard(*state_);
        if (!state_->requested) {
            state_->callbacks.push_back(slot);
            return cancellation_registration(state_, slot);
        }

        immediate_reason = state_->reason;
    }

    // Late registration is deterministic: once cancellation has been published, the
    // callback is invoked synchronously before register_callback returns. The state
    // lock is released first so user code can query, unregister, or register again.
    slot->registered = false;
    if (slot->callback && immediate_reason.has_value()) {
        slot->callback(*immediate_reason);
    }
    return {};
}

cancellation_source::cancellation_source()
    : state_(std::make_shared<detail::cancellation_state>()) {}

bool cancellation_source::can_request_cancellation() const noexcept {
    return state_ != nullptr;
}

cancellation_token cancellation_source::token() const noexcept {
    return cancellation_token(state_);
}

bool cancellation_source::cancellation_requested() const {
    return token().cancellation_requested();
}

std::optional<cancellation_reason> cancellation_source::reason() const {
    return token().reason();
}

bool cancellation_source::request_cancellation(cancellation_reason requested_reason) {
    if (!state_) {
        return false;
    }

    std::vector<std::shared_ptr<detail::cancellation_callback_slot>> callbacks;
    {
        detail::cancellation_state_lock guard(*state_);
        if (state_->requested) {
            return false;
        }

        state_->requested = true;
        state_->reason = requested_reason;
        callbacks.reserve(state_->callbacks.size());
        for (auto& slot : state_->callbacks) {
            if (slot && slot->registered) {
                slot->registered = false;
                callbacks.push_back(slot);
            }
        }
        state_->callbacks.clear();
    }

    // The first cancellation request wins while holding the internal mutex, but
    // callbacks run only after the mutex is released. An unregister that wins the
    // mutex before this selection removes its callback; after selection, the
    // callback is already owned by this local snapshot and may run exactly once.
    for (const auto& slot : callbacks) {
        if (slot->callback) {
            slot->callback(requested_reason);
        }
    }

    return true;
}

} // namespace voris::io
