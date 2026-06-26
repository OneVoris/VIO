# Hardening and Test Gates

Required release gates:

- Long cancellation, close, and shutdown stress tests.
- Normal Debug/Release CI on Linux and Windows for the non-sanitized test
  suite.
- TSan jobs for tasks, scopes, channels, mailboxes, and backends.
- ASan+UBSan jobs for Debug and Release builds.
- Backend contract suite for virtual, epoll, io_uring, kqueue, and IOCP where
  the platform supports them.
- Platform backend contract CI on macOS and Windows, with the kqueue and IOCP
  contract paths named in the runner matrix and unsupported-path targets still
  executed for visibility.
- End-to-end overload and memory ceiling tests.

These gates are requirements, not standing proof for a future release
candidate. Evidence commit `ea0d6568a37c35f5efa63f57c924b1fa8d6a8d66` records
the completed platform contract, Debug/Release, ASan+UBSan, and TSan gates via
GitHub Actions runs `28219130856`, `28219130885`, `28219130878`, and
`28219130861`. Future release candidates must record fresh evidence before
reusing the same hardening Definition of Done status.

## Debug/Release Evidence

The `Debug Release` GitHub Actions workflow is the normal non-sanitized
evidence path. It runs debug and release builds on `ubuntu-latest` and
`windows-latest`, registers and updates VXrepo, records
`xrepo info voris-vmem` before configuration, validates the repository, and
runs `xmake f -m <mode> --build_tests=y`, `xmake`, and `xmake test`.

The workflow entry does not complete the hardening Definition of Done by
itself. Evidence commit `ea0d6568a37c35f5efa63f57c924b1fa8d6a8d66` recorded
`Debug Release` run `28219130885`; each future release candidate must record
runner pass evidence across Debug, Release, ASan+UBSan, and TSan.

## End-to-End Overload Gate

`vio_e2e_overload_test` is the `VIO-M8-004` gate for scheduler lag, overload
recovery, and bounded memory pressure:

```bash
xmake run vio_e2e_overload_test
```

The scheduler-lag scenario runs against an actual shard loop and records
backlog delay through `runtime_metrics.scheduler_lag`. The overload-recovery
scenario proves a full bounded shard queue reports `resource_exhausted`, drains
accepted work, and then accepts later work.

There is no RSS hard-limit API in VIO. The memory ceiling proxy for M8 is the
owner-derived hard capacity on bounded queues, operation slots, channels, and
executors. The e2e gate exercises the bounded executor queue and reservation
path, verifies excess work returns `resource_exhausted`, and checks rejected
work neither runs later nor leaves persistent queue growth.

## ThreadSanitizer Coverage

The `ThreadSanitizer` GitHub Actions workflow registers VXrepo explicitly,
records `xrepo info voris-vmem`, verifies the GCC C++23 standard library,
builds the debug test suite on Linux with `--sanitize_thread=y`, then runs the
M8 TSan gate by category:

- tasks: `vio_task_test`, `vio_task_lifetime_test`, `vio_when_all_test`,
  `vio_when_any_test`;
- scopes: `vio_async_scope_test`;
- channels: `vio_channel_test`;
- mailboxes: `vio_mailbox_test`, `vio_shard_runtime_integration_test`;
- backends: `vio_backend_contract_test`, `vio_backend_differential_test`,
  `vio_epoll_backend_test`, `vio_io_uring_backend_test`,
  `vio_kqueue_backend_test`, `vio_iocp_backend_test`.

Local Linux GCC evidence uses the same configuration:

```bash
xmake f -m debug --build_tests=y --sanitize_thread=y --cc=gcc --cxx=g++
xmake
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

TSan is a Linux GCC/gcc-like gate for this repository and is not combined with
ASan/UBSan in the same build. Platform-specific backend tests may report an
unsupported-platform skip on Linux; release evidence still records the target,
command, exit status, and skip or pass output.

## ASan+UBSan Coverage

The `ASan UBSan` GitHub Actions workflow is the repository entry point for
AddressSanitizer and UndefinedBehaviorSanitizer evidence. It runs on
`ubuntu-latest` with the runner GCC C++ toolchain, installs latest xmake,
verifies `std::expected`/`std::unexpected`, registers VXrepo, records
`xrepo info voris-vmem`, and runs both debug and release test configurations:

```bash
xmake f -m debug --build_tests=y --sanitize_address_undefined=y --cc=gcc --cxx=g++
xmake
xmake test

xmake f -m release --build_tests=y --sanitize_address_undefined=y --cc=gcc --cxx=g++
xmake
xmake test
```

The `sanitize_address_undefined` option is intended for Linux GCC/gcc-like
toolchains. It is mutually exclusive with `sanitize_thread`, and xmake rejects a
configuration that attempts to enable both sanitizer families. Keep ASan+UBSan
and TSan in separate CI jobs and local build directories so their runtimes do
not conflict.

This workflow does not complete release hardening by itself. It creates a
repeatable evidence path; evidence commit
`ea0d6568a37c35f5efa63f57c924b1fa8d6a8d66` recorded `ASan UBSan` run
`28219130878`, and future release candidates must record their own Debug,
Release, ASan+UBSan, and TSan results.

Benchmark records include commit, compiler, standard library, flags, CPU,
operating system/kernel, workload, throughput, latency percentiles, peak RSS,
allocations per operation, and timeout/error counts.

## Long Race Stress

`vio_hardening_stress_test` is the VIO-M8-001 hardening gate for cancellation,
close, and shutdown races. The default `xmake test` mode is bounded and runs a
meaningful quick stress pass. It covers:

- cancellation versus backend completion, including exactly-once observation;
- close versus backend completion, including generation reuse and stale events;
- shutdown versus backend completion;
- virtual backend close/shutdown permutations;
- runtime submit versus shutdown races with bounded queues.

Long mode is opt-in so regular test runs remain fast:

```bash
VIO_HARDENING_STRESS_MODE=long VIO_HARDENING_STRESS_SECONDS=120 xmake run vio_hardening_stress_test
```

On Windows `cmd.exe`:

```bat
set VIO_HARDENING_STRESS_MODE=long&& set VIO_HARDENING_STRESS_SECONDS=120&& xmake run vio_hardening_stress_test
```

`VIO_HARDENING_STRESS_ITERATIONS` overrides the minimum iteration count per
stress family. `VIO_HARDENING_STRESS_SECONDS` is also a per-stress-family time
budget, not a total process timeout. Both variables must be unsigned decimal
digits with no leading sign. Long mode defaults to 60 seconds per family and
1024 minimum iterations; quick mode defaults to 128 iterations and no duration
floor. The test prints one evidence line with `mode`, `iterations`,
`duration_seconds`,
`cancellation_iterations`, `close_iterations`, `shutdown_iterations`,
`backend_iterations`, `runtime_iterations`, and elapsed milliseconds for each
family.
