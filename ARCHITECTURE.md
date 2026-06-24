# VIO Architecture

## 1. Boundary

C++23 coroutine runtime, per-core shard scheduler, cancellation, timers, asynchronous primitives, and portable I/O backends.

### In Scope

- Guarantee exactly-once completion for every asynchronous operation.
- Resume continuations on a defined scheduler rather than an accidental callback thread.
- Provide structured concurrency, cancellation, deadlines, and deterministic shutdown.
- Use bounded queues and explicit backpressure throughout the runtime.
- Present one operation contract across readiness and completion operating-system APIs.

### Out of Scope

- Providing TCP, HTTP, QUIC, cache, or database semantics.
- Using an unbounded global work-stealing pool for network I/O.
- Exposing epoll, io_uring, kqueue, or IOCP types in public APIs.

## 2. Dependency Boundary

| Dependency | Kind | Version policy |
|---|---|---|
| `voris-vmem` | Required | Latest released VXrepo package; no explicit version selector in this repository. |
| C++ standard library | Required | C++23 baseline. |
| Test/benchmark tools | Development only | Must not leak into the public ABI. |

The repository consumes only released public upstream APIs through VXrepo. Private headers, source copying, and relative cross-repository include paths are prohibited. The actual resolved `voris-vmem` version is build evidence from `xrepo info voris-vmem`; it is not pinned in the repository.

## 3. Component Model

| Component | Responsibility |
|---|---|
| Task model | Move-only coroutine tasks, promises, continuations, and composition. |
| Structured concurrency | Async scopes, cancellation trees, deadlines, and error aggregation. |
| Shard runtime | Per-core ready queues, mailboxes, budgets, and lifecycle. |
| I/O operation layer | Exactly-once state machines and backend-neutral handles. |
| Backends | Deterministic test backend, epoll, io_uring, kqueue, and IOCP. |
| Services | Timers, channels, semaphores, mutexes, blocking executor, and files. |

## 4. Primary Data Path

```text
operation creation → owner shard → backend submit/readiness → exactly-once arbitration → scheduled continuation
```

## 5. Core Invariants

- Each operation schedules at most one continuation.
- Operation storage remains alive until the backend can no longer reference it.
- User coroutine code never resumes while an internal backend lock is held.
- Tasks spawned into a scope cannot silently outlive that scope.
- Every queue is bounded or derives a strict maximum from an owner limit.
- Runtime shutdown drains or explicitly aborts every task, operation, timer, and thread.

## 6. Public API Direction

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
template<class... A> auto when_any(A&&...);

} // namespace voris::io
```

Public interfaces use C++23, move-only ownership where appropriate, `std::expected`-style explicit runtime errors, `std::span`/views for borrowing, and `std::chrono` for time. Provider or operating-system objects remain behind private adapters.

## 7. Error and Resource Model

- Ordinary runtime failures are values, not exceptions crossing subsystem boundaries.
- Error categories are stable identifiers; diagnostic text is not an API contract.
- Peer-controlled sizes and work queues have explicit hard limits.
- Cancellation, deadlines, and shutdown have documented ownership and completion behavior.
- Metrics and event hooks are narrow callbacks and do not create a hard logging dependency.

## 8. Concurrency and Lifetime

- Types declare whether they are thread-safe, shard-confined, immutable, or externally synchronized.
- A view never extends the lifetime of its source.
- No user callback or coroutine is resumed while a private lock protects mutable invariants.
- Cross-shard or cross-thread transfer is explicit and includes ownership transfer.
- Destruction cannot race with a still-referencing backend, waiter, callback, or provider.

## 9. Testing Contract

- Submit, cancel, timeout, close, completion, and shutdown race permutations.
- One contract suite against deterministic and operating-system backends.
- Scheduler fairness, lag, mailbox saturation, and overload recovery.
- File I/O short progress, blocking-pool saturation, and durability barriers.
- Long TSan stress for tasks, scopes, channels, timers, and native handles.

## 10. Security Review Areas

- Use-after-free in cancellation and native-handle reuse.
- Unbounded queues or task creation.
- Blocking the shard thread.
- Double completion and stale events.

## 11. Versioning

During `0.x`, source compatibility may change between minor versions. VIO follows the latest released `voris-vmem` package exposed by VXrepo and records the resolved version during validation. A public ABI promise begins only after a separately approved stability milestone.
