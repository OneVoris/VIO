# Public API Conventions

## Namespace and Include Layout

- Namespace: `voris::io`
- Include prefix: `#include <voris/io/...>`
- Primary target: `voris_vio`
- VXrepo package: `voris-vio`

## Language Baseline

The source baseline is C++23. Public headers may use concepts, `std::expected`, `std::span`, `std::string_view`, `std::chrono`, `std::source_location`, and move-only callables when they clarify ownership or constraints.

## Error Model

- Normal failures use explicit result values: `io_result<T>` is an alias for
  `std::expected<T, vio_error>`, and `void_result` is an alias for
  `std::expected<void, vio_error>`.
- `vio_error` is the public runtime error value. It carries a stable
  `vio_error_code` classification, an optional provider error code, diagnostic
  text for logging only, and a source location captured by helper construction.
- Stable error enum values are never reordered or reused after release.
- Platform/provider error codes may be attached for diagnostics but are not the only public classification.
- Error equality and ordering use only the stable classification and provider code.
  Diagnostic text and source location are not API-contract data and must not
  drive control flow.
- Invariant violations and invalid external input are distinct categories.

## Ownership

- `*_view`, `span`, and `string_view` are non-owning.
- `unique_*` is move-only ownership.
- `shared_*` explicitly extends lifetime; hot paths may use intrusive reference counting.
- `handle` is move-only and closes on destruction.
- Every view documents invalidation conditions.
- Public task owners are move-only and are not thread-safe unless a specific API
  documents otherwise. Destruction, moves, result extraction, and awaiter resume
  are confined to the owning scheduler/shard or must be externally synchronized.

## Asynchronous Behavior

Asynchronous APIs return VIO tasks and preserve the owner scheduler.

- Completion never changes thread or shard merely because an operation completed synchronously.
- Cancellation is a request with defined completion semantics, not permission to destroy live operation state.
- Deadlines use a monotonic clock.
- A public task cannot silently outlive its owning scope.
- Cross-thread task ownership transfer must be explicit through a scheduler or
  ownership-transfer API; callers must not race task owner destruction with a
  scheduler continuation that can resume the same frame.

### Task Combinators

`when_any(cancellation_source& losers_source, tasks...)` returns a
`task<when_any_result<task_i::result_type...>>`. The first child task to reach a
terminal state wins. A terminal state includes a successful result and an error
result stored in `io_result<T>` or `void_result`; an error result is still the
winning observation when it arrives first.

After a winner is observed, `when_any` requests
`cancellation_reason::manual` on the caller-provided `losers_source`. This is a
request to losing operations, not permission to destroy them. The source is
copied into the returned task's shared state, so the cancellation state remains
available even if the caller's source object goes out of scope. `when_any` keeps
observing all losing child tasks and does not complete its returned task until
every observer has reached a terminal state. A loser that ignores cancellation
keeps `when_any` pending until it completes naturally.

Abandoning the returned `when_any` task detaches its parent continuation. Later
child completions or queued resume work must not resume the destroyed parent
frame.

## Buffers

Borrowed byte ranges use VMem-compatible views. APIs document whether input is borrowed, consumed, retained, or copied. Scatter/gather operating-system types stay private.

## Configuration

Configuration is represented by validated value objects with safe hard limits. Avoid positional Boolean parameters; use named options and enums.

## ABI

The `0.x` series does not promise binary compatibility. Shared-library builds hide non-public symbols. Stable provider/plugin boundaries use PImpl or a versioned C ABI rather than exposing STL, exceptions, RTTI, or compiler-specific coroutine objects.

## API Sketch

```cpp
namespace voris::io {

template<class T = void> class task;
class scheduler_ref;
class async_scope;
class cancellation_token;
class cancellation_source;
class channel_base;

struct deadline {
    static deadline none() noexcept;
};

template<class... A> auto when_all(A&&...);
template<class... A> auto when_any(cancellation_source& losers_source, A&&...);

} // namespace voris::io
```
