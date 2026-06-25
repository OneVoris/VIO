# VIO TODO List

Task identifiers follow `VIO-M<milestone>-<sequence>`. A task is complete only when its tests, documentation, and required benchmarks are included.

## M0 — Semantics and a Testable Kernel

- [x] **VIO-M0-001** Create the `voris_vio` target, repository layout, and public error types.
- [x] **VIO-M0-002** Write an ADR for eager versus lazy tasks, continuation scheduling, destruction, and exception semantics.
- [x] **VIO-M0-003** Implement move-only `task<T>` and `task<void>` promise types.
- [x] **VIO-M0-004** Implement `scheduler_ref` and current-scheduler lookup.
- [x] **VIO-M0-005** Implement a trampoline that prevents unbounded recursion from synchronous completion.
- [x] **VIO-M0-006** Implement a deterministic single-thread test scheduler.
- [x] **VIO-M0-007** Implement a virtual monotonic clock.
- [x] **VIO-M0-008** Add tests for task lifetime, exceptions, moves, empty tasks, and repeated await attempts.

## M1 — Cancellation, Deadlines, and Structured Concurrency

- [x] **VIO-M1-001** Define cancellation reasons and token/source APIs.
- [x] **VIO-M1-002** Implement cancellation callback registration, including unregister-versus-callback races.
- [x] **VIO-M1-003** Link deadlines and cancellation propagation.
- [x] **VIO-M1-004** Write an ADR for the exactly-once operation-completion state machine.
- [x] **VIO-M1-005** Implement `async_scope::spawn`, `join`, and `request_stop`.
- [x] **VIO-M1-006** Implement scope error aggregation and a background-task error sink.
- [x] **VIO-M1-007** Implement `when_all`.
- [x] **VIO-M1-008** Implement `when_any` and define cancellation and observation of losing operations.
- [x] **VIO-M1-009** Add stress tests for cancellation before submit, during completion, and after completion.

## M2 — Shard Runtime and Scheduling

- [x] **VIO-M2-001** Implement shard threads, thread-local context, and shard lifecycle.
- [x] **VIO-M2-002** Implement bounded ready queues and cross-shard mailboxes.
- [x] **VIO-M2-003** Implement cross-shard `submit_to` with move-only messages.
- [x] **VIO-M2-004** Implement the backend wake-up abstraction.
- [x] **VIO-M2-005** Implement per-loop task, completion, and time budgets.
- [x] **VIO-M2-006** Implement scheduler-lag, queue-depth, and long-task counters.
- [x] **VIO-M2-007** Implement the runtime builder for shard count, CPU affinity, and queue limits.
- [ ] **VIO-M2-008** Implement an optional bounded compute executor.
- [ ] **VIO-M2-009** Test full mailboxes, shard stop, cross-shard return, and owner destruction.

## M3 — Timers and Asynchronous Primitives

- [ ] **VIO-M3-001** Implement an indexed timer heap and timer handles.
- [x] **VIO-M3-002** Implement `sleep_until` and `sleep_for`.
- [ ] **VIO-M3-003** Implement timer cancellation, same-deadline batching, and clock-jump protection.
- [x] **VIO-M3-004** Implement bounded `channel<T>` send, receive, and close.
- [ ] **VIO-M3-005** Add cancellation and deadlines to channel waiters.
- [ ] **VIO-M3-006** Implement `async_semaphore`.
- [ ] **VIO-M3-007** Implement `async_mutex` without resuming waiters while holding its internal lock.
- [x] **VIO-M3-008** Implement a manual-reset event.
- [x] **VIO-M3-009** Evaluate a timer wheel and change the default only if benchmarks show the heap is a bottleneck.

## M4 — Linux epoll Backend

- [x] **VIO-M4-001** Define the backend contract for register, submit, cancel, poll, wake, and shutdown.
- [x] **VIO-M4-002** Implement a generation-safe native-handle registry.
- [ ] **VIO-M4-003** Implement epoll polling and `eventfd` wake-up.
- [ ] **VIO-M4-004** Implement nonblocking `read_some` and `write_some`.
- [ ] **VIO-M4-005** Implement accept and connect, including `EINPROGRESS` and `SO_ERROR` handling.
- [x] **VIO-M4-006** Define and implement queueing rules for multiple read or write operations; reject undefined concurrent use.
- [ ] **VIO-M4-007** Implement close with pending-operation cancellation, stale-event defense, and file-descriptor reuse handling.
- [ ] **VIO-M4-008** Implement `readv` and `writev` buffer-chain adapters.
- [ ] **VIO-M4-009** Cover partial I/O, `EINTR`, `EAGAIN`, peer reset, and half-close.
- [ ] **VIO-M4-010** Run the same backend contract suite against the virtual backend and epoll.

## M5 — Files and Blocking Work

- [ ] **VIO-M5-001** Implement a bounded blocking executor whose full queue returns a resource error.
- [ ] **VIO-M5-002** Implement asynchronous file open, close, `read_at`, and `write_at`.
- [x] **VIO-M5-003** Implement file size, truncate, and allocation-hint operations.
- [x] **VIO-M5-004** Implement `sync_data` and `sync_all` and document platform durability semantics.
- [x] **VIO-M5-005** Define completion when a started blocking system call cannot be forcibly cancelled.
- [x] **VIO-M5-006** Expose sendfile-compatible access without leaking raw ownership.
- [ ] **VIO-M5-007** Test file-pool saturation, shutdown, short reads/writes, and disk errors.

## M6 — io_uring Backend

- [ ] **VIO-M6-001** Detect kernel features and opcodes and build a capability set.
- [ ] **VIO-M6-002** Implement submission/completion queue lifecycle and batched submit/poll.
- [ ] **VIO-M6-003** Implement socket read, write, accept, and connect operations.
- [ ] **VIO-M6-004** Implement file read, write, and `fsync` operations.
- [ ] **VIO-M6-005** Implement asynchronous cancellation and verify completion races.
- [ ] **VIO-M6-006** Add optional registered buffers and files without changing default ownership semantics.
- [ ] **VIO-M6-007** Add behavioral differential tests between epoll and io_uring.
- [ ] **VIO-M6-008** Define default-enable criteria with benchmarks and fall back when capabilities are insufficient.

## M7 — kqueue and IOCP

- [ ] **VIO-M7-001** Implement the kqueue backend and wake-up mechanism.
- [ ] **VIO-M7-002** Implement IOCP handle association and batched completion retrieval.
- [ ] **VIO-M7-003** Define `OVERLAPPED` operation lifetime and cancellation behavior.
- [ ] **VIO-M7-004** Run the complete backend contract suite on macOS and Windows.
- [x] **VIO-M7-005** Document cancellation and file-I/O semantics that cannot be made identical across platforms.

## M8 — Hardening, Benchmarks, and Release

- [ ] **VIO-M8-001** Run long-duration cancellation, close, and shutdown race stress tests.
- [ ] **VIO-M8-002** Add TSan jobs covering tasks, scopes, channels, mailboxes, and backends.
- [ ] **VIO-M8-003** Benchmark scheduler hops, task spawn, timers, channels, and socket ping-pong.
- [ ] **VIO-M8-004** Add end-to-end tests for scheduler lag, overload recovery, and memory ceilings.
- [ ] **VIO-M8-005** Provide echo, fan-out, file-copy, and graceful-shutdown examples.
- [ ] **VIO-M8-006** Document backend capabilities, limits, and failure modes.

## Definition of Done

- [ ] Exactly-once completion and continuation-scheduler semantics are demonstrated by tests.
- [ ] No blocking call runs on a shard; explicit executor exceptions are documented and tested.
- [ ] Every wait queue is bounded or has a strict owner-derived maximum.
- [ ] Virtual and operating-system backends satisfy the same contract.
- [ ] Debug, Release, ASan+UBSan, and TSan configurations pass.
- [ ] Shutdown leaves no pending operation, thread, or timer.
- [ ] Hot-path changes include latency, throughput, allocation, and queue-depth benchmarks.
