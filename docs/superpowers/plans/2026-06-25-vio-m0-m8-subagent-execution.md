# VIO M0-M8 Subagent Execution Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build VIO from the current scaffold through M0-M8 with supervised subagent execution, one independent commit per milestone.

**Architecture:** The main agent owns sequencing, review, integration, VXrepo evidence, and milestone commits. Worker subagents own disjoint file sets inside one workspace and must not cross milestone gates before public semantics and ADRs are accepted.

**Tech Stack:** C++23, XMake, VXrepo, `voris-vmem` from the latest released VXrepo package selector with no explicit version, `std::expected`, platform backends for epoll, io_uring, kqueue, and IOCP.

---

## Global Decisions

- `task<T>` is eager, move-only, and single-await. Repeated await completes with `vio_error_code::invalid_state`.
- Ordinary runtime failures use `std::expected<T, vio_error>` and `std::expected<void, vio_error>`.
- A missing current scheduler is an explicit `invalid_state` failure. VIO provides a default scheduler type, but callers must explicitly create or install it.
- Exactly-once arbitration uses first terminal completion wins. If cancellation wins, the first cancellation reason is retained.
- `when_any` cancels losing operations, drains them to observed terminal states, and then returns the winning result.
- Bounded queues use async wait for capacity. Wait queues themselves must be bounded or owner-derived.
- Runtime shutdown first requests graceful stop and drains work. A configured shutdown deadline moves remaining pending work to abort completion.
- `channel<T>` drains buffered values after close, supports zero-capacity rendezvous mode, and serves send/receive waiters FIFO.
- A native handle may queue same-direction I/O in FIFO order. Queue nodes are cancellable, bounded, and completed exactly once.
- A blocking file syscall that has already started reports the real syscall result even if cancellation was requested.
- Linux defaults to epoll until io_uring passes capability, race, and benchmark gates.
- Public license metadata is `GPL-3.0-only`, with a notice that separate commercial licenses are available.

## Main Agent Rules

- Work in milestone order. Do not start implementation for `M(n+1)` until the `Mn` commit exists and passes the milestone gate.
- Each milestone ends in exactly one milestone commit named `feat: complete VIO Mx <short-name>` or `docs: plan VIO Mx <short-name>` when the milestone only changes planning/documentation.
- Worker subagents may run in the same workspace only when their declared write sets do not overlap.
- Public Markdown, code comments, commit messages, and issue text are English.
- Agent-private notes and reports stay under `.agent/` and remain Chinese.
- Every task starts with a failing test, reproducible check, ADR draft, or contract test before implementation changes.
- The main agent runs the gate commands and performs spec and quality review before every milestone commit.
- VXrepo evidence is recorded before any task that consumes `voris-vmem`: repository remote, metadata revision, `xrepo info voris-vmem`, resolved version, package source, and command output.

## Common Gate Commands

Run these before each milestone commit unless the milestone-specific gate says to add more:

```powershell
python tools/check_repository.py
xmake f -m debug --build_tests=y
xmake
xmake test
xmake f -m release --build_tests=y
xmake
xmake test
```

When a gate cannot run in the current sandbox, the main agent records the command, reason, and required rerun environment in the milestone report.

## M0 Commit: Semantics and Testable Kernel

**Commit message:** `feat: complete VIO M0 semantics kernel`

**Milestone goal:** Establish public errors, task semantics, scheduler references, deterministic testing, and virtual time.

### M0 Package A: Dependency and Repository Baseline

**Owner:** Worker `M0-baseline`.

**Task IDs:** `VIO-M0-001`.

**Write set:**
- `xmake.lua`
- `voris-package.toml`
- `README.md`
- `ARCHITECTURE.md`
- `docs/BUILDING.md`
- `docs/DEPENDENCIES.md`
- `docs/REPOSITORY_ISOLATION.md`
- `.agent/UPSTREAM_SYNC.md`
- `.agent/UPSTREAM_REQUEST_TEMPLATE.md`

**Instructions:**
- Remove the explicit XMake version requirement from public build configuration.
- Replace `add_requires("voris-vmem >=0.1.0 <0.2.0")` with the no-version selector `add_requires("voris-vmem")`.
- Remove the `with_voris_dependencies` option as an implementation gate; VMem is a required package from M0 onward.
- Keep `voris-vmem` public only if public headers include or expose VMem types.
- Remove dependency version ranges from `voris-package.toml`.
- Update public docs to say VXrepo is already the source of released packages and VIO consumes the latest released `voris-vmem` without an explicit version.
- Update `.agent` templates to record selector policy as `latest release / no explicit version` plus the resolved version reported by `xrepo info`.

**Required checks:**
```powershell
python tools/check_repository.py
xrepo info voris-vmem
xmake f -m debug --build_tests=y
xmake
xmake test
```

### M0 Package B: Task Semantics ADR

**Owner:** Worker `M0-adr`.

**Task IDs:** `VIO-M0-002`.

**Write set:**
- `docs/adr/0001-task-scheduler-semantics.md`

**Instructions:**
- Document eager task start, coroutine frame ownership, move-only ownership, destruction rules, `initial_suspend`, `final_suspend`, continuation scheduling, exception capture, empty task behavior, and repeated await behavior.
- State that user code is never resumed while an internal lock is held.
- State that all continuations resume through an explicit scheduler.
- Verification section must name tests for lifetime, exception capture, moves, empty tasks, repeated await, and synchronous-completion trampoline behavior.

### M0 Package C: Error and Result API

**Owner:** Worker `M0-errors`.

**Task IDs:** `VIO-M0-001`.

**Write set:**
- `include/voris/io/error.hpp`
- `src/error.cpp`
- `tests/error_test.cpp`
- `xmake.lua`
- `docs/API.md`

**Instructions:**
- Add `enum class vio_error_code` with at least `none`, `invalid_state`, `cancelled`, `deadline_exceeded`, `resource_exhausted`, `operation_in_progress`, `closed`, `backend_failure`, and `unsupported`.
- Add `struct vio_error` with stable classification, optional provider code, and diagnostic text for logging only.
- Add `template<class T> using io_result = std::expected<T, vio_error>`.
- Add a `void_result` alias as `std::expected<void, vio_error>`.
- Tests must verify stable equality by classification and that diagnostic text is not needed for equality.

### M0 Package D: Task and Scheduler Core

**Owner:** Worker `M0-task-core`.

**Task IDs:** `VIO-M0-003`, `VIO-M0-004`, `VIO-M0-005`.

**Write set:**
- `include/voris/io/task.hpp`
- `include/voris/io/scheduler.hpp`
- `include/voris/io/trampoline.hpp`
- `src/scheduler.cpp`
- `src/trampoline.cpp`
- `tests/task_test.cpp`
- `tests/scheduler_test.cpp`
- `xmake.lua`

**Instructions:**
- Implement eager `task<T>` and `task<void>` with move-only ownership.
- Implement single-await state tracking and return `invalid_state` for repeated await attempts.
- Capture coroutine exceptions and surface them through the task result channel.
- Implement `scheduler_ref` as a non-owning reference to a scheduler vtable that can enqueue move-only continuations.
- Implement explicit current scheduler install/restore scope.
- Implement a synchronous-completion trampoline with a bounded recursion depth and FIFO deferred continuation drain.

### M0 Package E: Deterministic Test Scheduler and Virtual Clock

**Owner:** Worker `M0-test-kernel`.

**Task IDs:** `VIO-M0-006`, `VIO-M0-007`, `VIO-M0-008`.

**Write set:**
- `include/voris/io/test_scheduler.hpp`
- `include/voris/io/virtual_clock.hpp`
- `tests/test_scheduler_test.cpp`
- `tests/virtual_clock_test.cpp`
- `tests/task_lifetime_test.cpp`
- `xmake.lua`

**Instructions:**
- Add a deterministic single-thread scheduler that queues continuations and advances only when the test calls `run_one`, `run_ready`, or `run_until_idle`.
- Add a virtual monotonic clock with explicit `advance_by` and `advance_to`.
- Tests cover task lifetime, exceptions, move construction/assignment, empty tasks, repeated await, scheduler restoration, and trampoline recursion safety.

**M0 additional gate:**
```powershell
xmake f -m debug --build_tests=y --build_examples=n --build_benchmarks=n --build_fuzzers=n
xmake
xmake test
```

## M1 Commit: Cancellation, Deadlines, and Structured Concurrency

**Commit message:** `feat: complete VIO M1 structured concurrency`

**Milestone goal:** Add cancellation, deadlines, exactly-once completion, async scopes, and task combinators.

### M1 Packages

- `M1-cancel`: implement `include/voris/io/cancellation.hpp`, `src/cancellation.cpp`, and `tests/cancellation_test.cpp` for reason ordering, source/token ownership, callback registration, unregister race, and first-cancel reason retention. Task IDs: `VIO-M1-001`, `VIO-M1-002`.
- `M1-deadline`: implement `include/voris/io/deadline.hpp`, `src/deadline.cpp`, and `tests/deadline_test.cpp` for monotonic deadline representation and cancellation propagation. Task ID: `VIO-M1-003`.
- `M1-operation-adr`: write `docs/adr/0002-exactly-once-operation-completion.md` covering submit, cancel, backend complete, close, timeout, storage lifetime, and scheduler restoration. Task ID: `VIO-M1-004`.
- `M1-scope`: implement `include/voris/io/async_scope.hpp`, `src/async_scope.cpp`, and `tests/async_scope_test.cpp` for `spawn`, `join`, `request_stop`, graceful drain, abort deadline, error aggregation, and background-task sink. Task IDs: `VIO-M1-005`, `VIO-M1-006`.
- `M1-combinators`: implement `include/voris/io/when_all.hpp`, `include/voris/io/when_any.hpp`, `tests/when_all_test.cpp`, and `tests/when_any_test.cpp`; `when_any` cancels and drains losers. Task IDs: `VIO-M1-007`, `VIO-M1-008`.
- `M1-stress`: add `tests/cancellation_stress_test.cpp` for before-submit, during-completion, after-completion, and repeated cancellation races. Task ID: `VIO-M1-009`.

**M1 additional gate:**
```powershell
xmake f -m debug --build_tests=y
xmake
xmake test
```

## M2 Commit: Shard Runtime and Scheduling

**Commit message:** `feat: complete VIO M2 shard runtime`

**Milestone goal:** Provide shard threads, bounded scheduling queues, cross-shard submission, wakeups, budgets, metrics, and runtime construction.

### M2 Packages

- `M2-lifecycle`: `include/voris/io/runtime.hpp`, `include/voris/io/shard.hpp`, `src/runtime.cpp`, `src/shard.cpp`, `tests/shard_lifecycle_test.cpp`. Task ID: `VIO-M2-001`.
- `M2-queues`: `include/voris/io/detail/bounded_queue.hpp`, `include/voris/io/detail/mailbox.hpp`, `tests/bounded_queue_test.cpp`, `tests/mailbox_test.cpp`; queues are bounded and full queues suspend submitters with bounded waiter ownership. Task ID: `VIO-M2-002`.
- `M2-submit`: `include/voris/io/submit.hpp`, `src/submit.cpp`, `tests/submit_to_test.cpp`; move-only cross-shard messages and return-to-owner behavior. Task ID: `VIO-M2-003`.
- `M2-wakeup-budget`: `include/voris/io/backend_wakeup.hpp`, `include/voris/io/loop_budget.hpp`, `src/backend_wakeup.cpp`, `tests/wakeup_budget_test.cpp`. Task IDs: `VIO-M2-004`, `VIO-M2-005`.
- `M2-metrics-builder`: `include/voris/io/runtime_options.hpp`, `include/voris/io/runtime_metrics.hpp`, `src/runtime_options.cpp`, `tests/runtime_builder_test.cpp`, `tests/runtime_metrics_test.cpp`. Task IDs: `VIO-M2-006`, `VIO-M2-007`.
- `M2-compute`: `include/voris/io/compute_executor.hpp`, `src/compute_executor.cpp`, `tests/compute_executor_test.cpp`; optional bounded compute executor with async capacity waits and shutdown drain. Task ID: `VIO-M2-008`.
- `M2-integration`: `tests/shard_runtime_integration_test.cpp` for full mailbox, shard stop, cross-shard return, owner destruction. Task ID: `VIO-M2-009`.

## M3 Commit: Timers and Asynchronous Primitives

**Commit message:** `feat: complete VIO M3 timers and primitives`

**Milestone goal:** Add timer services, sleep, channels, semaphore, mutex, manual-reset event, and timer benchmark decision.

### M3 Packages

- `M3-timers`: `include/voris/io/timer.hpp`, `src/timer.cpp`, `tests/timer_test.cpp`; indexed heap, handles, cancel/fired races, same-deadline batching, clock-jump protection. Task IDs: `VIO-M3-001`, `VIO-M3-002`, `VIO-M3-003`.
- `M3-channel`: `include/voris/io/channel.hpp`, `tests/channel_test.cpp`; bounded buffer, zero-capacity rendezvous, close-drain, FIFO waiters, cancellation and deadlines. Task IDs: `VIO-M3-004`, `VIO-M3-005`.
- `M3-semaphore`: `include/voris/io/async_semaphore.hpp`, `tests/async_semaphore_test.cpp`; FIFO waiters, cancellation, deadlines. Task ID: `VIO-M3-006`.
- `M3-mutex-event`: `include/voris/io/async_mutex.hpp`, `include/voris/io/manual_reset_event.hpp`, `tests/async_mutex_test.cpp`, `tests/manual_reset_event_test.cpp`; never resume user code under internal locks. Task IDs: `VIO-M3-007`, `VIO-M3-008`.
- `M3-benchmark`: `benchmarks/timer_heap_benchmark.cpp`, `benchmarks/README.md`, and `docs/adr/0003-timer-heap-versus-wheel.md`; keep heap default unless benchmark evidence proves bottleneck. Task ID: `VIO-M3-009`.

## M4 Commit: Linux epoll Backend

**Commit message:** `feat: complete VIO M4 epoll backend`

**Milestone goal:** Define backend contract and implement Linux epoll with eventfd wakeup, queued socket operations, close safety, and contract tests.

### M4 Packages

- `M4-contract`: `include/voris/io/backend.hpp`, `tests/backend_contract_test.cpp`, `docs/adr/0004-backend-contract.md`. Task IDs: `VIO-M4-001`, `VIO-M4-010`.
- `M4-registry`: `include/voris/io/detail/native_handle_registry.hpp`, `src/native_handle_registry.cpp`, `tests/native_handle_registry_test.cpp`. Task ID: `VIO-M4-002`.
- `M4-epoll-loop`: `src/backends/epoll_backend.cpp`, `include/voris/io/backends/epoll_backend.hpp`, `tests/epoll_backend_test.cpp`. Task ID: `VIO-M4-003`.
- `M4-socket-ops`: `include/voris/io/socket.hpp`, `src/socket.cpp`, `tests/socket_io_test.cpp`; nonblocking read/write/accept/connect and queued same-direction I/O. Task IDs: `VIO-M4-004`, `VIO-M4-005`, `VIO-M4-006`.
- `M4-close-vectors`: close cancellation, stale event defense, fd reuse, `readv` and `writev` adapters, partial I/O, `EINTR`, `EAGAIN`, reset, half-close tests. Task IDs: `VIO-M4-007`, `VIO-M4-008`, `VIO-M4-009`.

## M5 Commit: Files and Blocking Work

**Commit message:** `feat: complete VIO M5 files and blocking work`

**Milestone goal:** Provide bounded blocking execution and asynchronous file APIs with clear cancellation and durability semantics.

### M5 Packages

- `M5-blocking-executor`: `include/voris/io/blocking_executor.hpp`, `src/blocking_executor.cpp`, `tests/blocking_executor_test.cpp`; bounded workers and bounded async capacity waits. Task ID: `VIO-M5-001`.
- `M5-file-basic`: `include/voris/io/file.hpp`, `src/file.cpp`, `tests/file_io_test.cpp`; open, close, `read_at`, `write_at`. Task ID: `VIO-M5-002`.
- `M5-file-metadata`: file size, truncate, allocation hints, sync_data, sync_all, platform durability docs. Task IDs: `VIO-M5-003`, `VIO-M5-004`.
- `M5-cancel-sendfile`: started blocking syscall reports actual result; sendfile-compatible borrowed access without raw ownership transfer. Task IDs: `VIO-M5-005`, `VIO-M5-006`.
- `M5-tests`: file-pool saturation, shutdown, short reads/writes, disk errors. Task ID: `VIO-M5-007`.

## M6 Commit: io_uring Backend

**Commit message:** `feat: complete VIO M6 io_uring backend`

**Milestone goal:** Add an optional io_uring backend that satisfies the shared backend contract and remains non-default until proven.

### M6 Packages

- `M6-capabilities`: capability and opcode detection, fallback reasons, tests. Task ID: `VIO-M6-001`.
- `M6-ring-lifecycle`: SQ/CQ lifecycle, batched submit/poll, shutdown, tests. Task ID: `VIO-M6-002`.
- `M6-socket-file-ops`: socket read/write/accept/connect and file read/write/fsync through io_uring. Task IDs: `VIO-M6-003`, `VIO-M6-004`.
- `M6-cancel-resources`: async cancellation races, optional registered buffers/files without default ownership changes. Task IDs: `VIO-M6-005`, `VIO-M6-006`.
- `M6-differential`: epoll versus io_uring behavioral tests and default-enable benchmark criteria. Task IDs: `VIO-M6-007`, `VIO-M6-008`.

## M7 Commit: kqueue and IOCP

**Commit message:** `feat: complete VIO M7 kqueue and iocp backends`

**Milestone goal:** Add macOS kqueue and Windows IOCP backends and document unavoidable platform differences.

### M7 Packages

- `M7-kqueue`: kqueue backend and wakeup mechanism with backend contract tests. Task ID: `VIO-M7-001`.
- `M7-iocp`: IOCP association, batched completion retrieval, queued operations, and shutdown. Task ID: `VIO-M7-002`.
- `M7-overlapped`: `OVERLAPPED` operation lifetime, cancellation, close races, stale completion tests. Task ID: `VIO-M7-003`.
- `M7-ci-contract`: run backend contract suite on macOS and Windows. Task ID: `VIO-M7-004`.
- `M7-platform-docs`: document cancellation and file-I/O differences that cannot be made identical. Task ID: `VIO-M7-005`.

## M8 Commit: Hardening, Benchmarks, and Release

**Commit message:** `feat: complete VIO M8 hardening and release`

**Milestone goal:** Harden the runtime, publish benchmark evidence, provide examples, and complete release documentation.

### M8 Packages

- `M8-stress`: long-duration cancellation, close, and shutdown race stress tests. Task ID: `VIO-M8-001`.
- `M8-tsan`: TSan jobs for tasks, scopes, channels, mailboxes, and backends. Task ID: `VIO-M8-002`.
- `M8-benchmarks`: scheduler hops, task spawn, timers, channels, socket ping-pong benchmark targets and record format. Task ID: `VIO-M8-003`.
- `M8-e2e`: scheduler lag, overload recovery, and memory ceiling tests. Task ID: `VIO-M8-004`.
- `M8-examples`: echo, fan-out, file-copy, and graceful-shutdown examples with build coverage. Task ID: `VIO-M8-005`.
- `M8-release-docs`: backend capabilities, limits, failure modes, license metadata, commercial-license notice, changelog, and release checklist. Task ID: `VIO-M8-006`.

## Final Completion Audit

- Every `VIO-M0-*` through `VIO-M8-*` item is checked off in `TODO.md` only after tests, docs, and required measurements are committed.
- `python tools/check_repository.py` passes.
- Debug and Release builds pass.
- ASan+UBSan and TSan evidence is recorded.
- Backend contract suite passes for virtual, epoll, io_uring, kqueue, and IOCP where supported by the platform.
- Benchmarks include commit, compiler, standard library, flags, CPU, OS/kernel, workload, throughput, latency percentiles, peak RSS, allocations per operation, and timeout/error counts.
