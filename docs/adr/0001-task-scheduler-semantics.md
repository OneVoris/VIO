# ADR 0001 — Task and Scheduler Semantics

- Status: Proposed
- Date: 2026-06-25
- Owners: repository maintainers
- Related tasks: VIO-M0-002

## Context

VIO tasks are the public coroutine boundary for asynchronous operations, structured concurrency, timers, and backend completions. The runtime must make task lifetime, continuation scheduling, and failure behavior deterministic before the task and scheduler implementation begins.

The primary invariant is that every asynchronous operation completes at most once and resumes on a defined scheduler. Task semantics must therefore avoid accidental callback-thread resumption, ambiguous ownership of coroutine frames, implicit scheduler fallbacks, and recursion hazards from chains of synchronous completions.

## Decision

`task<T>` and `task<void>` are eager, move-only, and single-await.

### Coroutine Frame Ownership

A non-empty `task<T>` or `task<void>` is the unique owner of its coroutine frame. Copy construction and copy assignment are disabled. Move construction and move assignment transfer the frame handle and all observation rights to the destination; the source becomes empty.

Calling `operator co_await() &&` consumes the task. Frame ownership transfers from the task object to the awaiter object stored in the awaiting coroutine frame. The original task becomes empty. The awaiter owns the child frame until `await_resume` consumes the result and destroys the frame, or until the awaiting coroutine is itself destroyed.

An empty task is a valid moved-from state. Destroying an empty task is a no-op. Awaiting an empty task completes with `vio_error_code::invalid_state`.

Destroying a non-empty task before it has been awaited abandons the task. The destructor does not block, does not resume user code, and does not silently detach a continuation. It destroys the owned coroutine frame and requires any active awaiter or operation object in that frame to unregister or release backend references safely. Later backend completion for abandoned work must not target the destroyed frame.

Destroying a completed task destroys only the completed coroutine frame and stored result or error.

### Creation and Start Timing

Task creation starts the coroutine eagerly. The task promise captures the current scheduler before user coroutine body code runs. If no current scheduler is installed, the coroutine body is not entered; the task completes with `vio_error_code::invalid_state`.

VIO may provide a default scheduler type, but there is no implicit global default. Callers must explicitly create, install, or otherwise pass the scheduler that defines where task code and continuations run.

### `initial_suspend`

`initial_suspend` is an eager-start guard:

- when a current scheduler is available, it does not suspend and the coroutine body starts immediately;
- when no current scheduler is available, it records `vio_error_code::invalid_state` and prevents entry into the user coroutine body.

This preserves eager execution while keeping missing scheduler state deterministic.

### `final_suspend`

`final_suspend` always suspends the completing coroutine frame. It never resumes a continuation inline. If a continuation was installed, the final awaiter schedules that continuation through the stored continuation scheduler using the runtime trampoline. If no continuation was installed, the completed frame remains owned by the task or awaiter until destruction.

### Continuation Storage

Each task promise contains at most one continuation slot. The slot stores:

- the awaiting coroutine handle;
- the scheduler captured from the awaiting coroutine context;
- a consumed flag that prevents a second await attempt.

The first valid await attempt installs the continuation exactly once. A repeated await attempt, including awaiting a moved-from or already-consumed task, completes with `vio_error_code::invalid_state`.

### Scheduler Capture and Restoration

Each task promise stores the scheduler captured at task creation. Every resume of that task installs the captured scheduler as the current scheduler for the dynamic extent of the resume and restores the previous current scheduler before returning to the scheduler loop, trampoline, or caller.

When a task is awaited, the awaiter captures the awaiting coroutine's current scheduler and stores it with the continuation. Completion of the child task schedules the awaiting coroutine on that captured continuation scheduler. If the awaiter cannot capture a current scheduler, the await operation completes with `vio_error_code::invalid_state`.

All continuation resumes go through an explicit scheduler. Backend callbacks and internal helper threads may request scheduling, but they do not directly resume user coroutine code.

### Result and Exception Behavior

Successful `task<T>` completion stores the produced `T`. Successful `task<void>` completion stores a void success. Runtime failures are reported through the task result channel as `vio_error`.

Unhandled exceptions escaping the coroutine body are caught by `promise_type::unhandled_exception` and converted to `vio_error_code::invalid_state`. Diagnostic text may record the exception type or message for logging, but callers must not depend on diagnostic text for control flow.

Awaiting an empty, moved-from, or already-consumed task also completes with `vio_error_code::invalid_state`.

### Locks, Backend Callbacks, and User Code

VIO never resumes user coroutine code while holding an internal mutex, scheduler queue lock, backend registry lock, or operation state lock. VIO also never resumes user coroutine code directly from a backend callback.

Internal code may publish completion state while holding a lock, but it must release that lock before enqueueing or running any continuation that can execute user code.

### Synchronous Completion Trampoline

Synchronous completion still schedules continuations through the runtime trampoline. The trampoline bounds recursive resume depth and drains deferred continuations in FIFO order. If a task completes while another continuation is already being resumed, the next continuation is deferred through the trampoline instead of recursively resuming inline.

## Alternatives Considered

Lazy tasks were rejected because they make start timing depend on the first awaiter and can hide missing scheduler installation until later in the call chain.

Copyable or multi-await tasks were rejected for M0 because they require shared result state, multi-consumer lifetime rules, and additional synchronization that is not needed for the initial task kernel.

Implicit default scheduler lookup was rejected because it can accidentally bind work to an unexpected thread or runtime instance. A default scheduler implementation may exist, but it must be explicitly installed or passed.

Inline final-suspend continuation resumption was rejected because synchronous completion chains can otherwise create unbounded recursion and can accidentally resume user code from callback or lock-owned paths.

## Consequences

Task ownership is simple and local: one non-empty task or awaiter owns one coroutine frame. Move-only, single-await semantics make repeated observation a deterministic `invalid_state` failure instead of shared mutable state.

Eager start means task functions perform work immediately under the current scheduler. Callers that need deferred start must use a separate explicit abstraction, such as a factory, scope spawn operation, or scheduler submission API.

Missing scheduler state is a normal, testable runtime failure. This avoids hidden fallback behavior but requires tests and examples to install a scheduler explicitly.

The implementation must make frame destruction safe for suspended awaiters and backend-owned operation state. Backend operation storage must not assume the coroutine frame remains alive after task abandonment.

The trampoline is part of the task contract, not an optimization detail. Tests must verify that synchronous completion chains do not grow the C++ call stack without bound.

## Verification

Required verification for VIO-M0 task and scheduler work:

- lifetime tests for eager start, normal completion, abandonment before await, completed-task destruction, and awaiter-owned frame destruction;
- exception capture tests showing unhandled coroutine exceptions become `vio_error_code::invalid_state`;
- move tests for move construction, move assignment, moved-from empty state, and ownership transfer before and after completion;
- empty task tests showing default-constructed or moved-from tasks await as `vio_error_code::invalid_state`;
- repeated await attempt tests showing a consumed task cannot be awaited a second time and reports `vio_error_code::invalid_state`;
- scheduler restoration tests showing each task resume installs its captured scheduler and restores the previous scheduler after resume;
- continuation scheduling tests showing awaiting coroutines resume on the scheduler captured at await time;
- backend callback and internal-lock tests showing user coroutine code is not resumed under callback execution or internal locks;
- trampoline recursion tests showing long chains of synchronous task completions run through bounded-depth FIFO trampoline draining.
