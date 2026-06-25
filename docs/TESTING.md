# Testing Strategy

## Test Layers

1. Unit tests for pure algorithms and value types.
2. Contract tests for public behavior and interchangeable backends/providers.
3. Integration tests for real operating-system, file, network, or dependency behavior.
4. Differential or interoperability tests against independent implementations where applicable.
5. Fuzz tests for parsers, codecs, formats, and state transitions.
6. Stress tests for cancellation, close, shutdown, concurrency, and resource pressure.
7. Benchmarks for throughput, p50/p95/p99/p99.9 latency, memory, allocations, and system calls.

## Repository-Specific Focus

- Submit, cancel, timeout, close, completion, and shutdown race permutations.
- One contract suite against deterministic and operating-system backends.
- Scheduler fairness, lag, mailbox saturation, and overload recovery.
- File I/O short progress, blocking-pool saturation, and durability barriers.
- Long TSan stress for tasks, scopes, channels, timers, and native handles.

## Required Configurations

- Debug.
- Release.
- ASan + UBSan.
- TSan for concurrent code.
- Fuzz configuration for parser/format targets.
- Tier-1 compiler and operating-system matrix.

## ThreadSanitizer

The TSan configuration is enabled explicitly and is intended for Linux clang
builds. Do not combine it with ASan/UBSan in the same build directory.

```bash
xmake f -m debug --build_tests=y --sanitize_thread=y --cc=clang --cxx=clang++
xmake
```

Run the same coverage set locally that CI runs:

```bash
xmake run vio_task_test
xmake run vio_task_lifetime_test
xmake run vio_when_all_test
xmake run vio_when_any_test
xmake run vio_async_scope_test
xmake run vio_channel_test
xmake run vio_mailbox_test
xmake run vio_shard_runtime_integration_test
xmake run vio_backend_contract_test
xmake run vio_backend_differential_test
xmake run vio_epoll_backend_test
xmake run vio_io_uring_backend_test
xmake run vio_kqueue_backend_test
xmake run vio_iocp_backend_test
```

The CI workflow registers VXrepo before configuring the build, then groups
those targets as tasks, scopes, channels, mailboxes, and backends. Record the
compiler, operating system/kernel, VXrepo registration step, `xmake f` line,
resolved `xrepo info voris-vmem` output, target list, and pass/skip/failure
result in hardening evidence. Backend targets that are unsupported on the Linux
runner must still be listed so their skip paths remain visible.

## Hardening Stress

The default hardening stress gate is included in `xmake test` through the
`hardening_stress` test target:

```bash
xmake run vio_hardening_stress_test
```

Default mode is intentionally bounded for local and CI debug runs. To run the
long cancellation, close, and shutdown race stress pass, set:

```bash
VIO_HARDENING_STRESS_MODE=long VIO_HARDENING_STRESS_SECONDS=120 xmake run vio_hardening_stress_test
```

Windows `cmd.exe` example:

```bat
set VIO_HARDENING_STRESS_MODE=long&& set VIO_HARDENING_STRESS_SECONDS=120&& xmake run vio_hardening_stress_test
```

`VIO_HARDENING_STRESS_SECONDS` is a per-stress-family budget, not a total
process timeout. `VIO_HARDENING_STRESS_ITERATIONS` sets the minimum iteration
budget per family. Both variables must be unsigned decimal digits with no
leading sign. Capture the printed `VIO_HARDENING_STRESS` evidence line in
release validation notes.

## Benchmark Record

Every reported result includes commit, compiler, standard library, flags, CPU, operating system/kernel, workload, throughput, latency percentiles, peak RSS, allocations per operation, and errors/timeouts. A single RPS number is not a release argument.

## Completion Rule

A TODO item is not complete until its failure paths, documentation, and required measurements are part of the same change.
