# Hardening and Test Gates

Required release gates:

- Long cancellation, close, and shutdown stress tests.
- TSan jobs for tasks, scopes, channels, mailboxes, and backends.
- ASan+UBSan jobs for Debug and Release builds.
- Backend contract suite for virtual, epoll, io_uring, kqueue, and IOCP where
  the platform supports them.
- End-to-end overload and memory ceiling tests.

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
