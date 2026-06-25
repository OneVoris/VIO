# Benchmarks

Benchmarks record environment, workload, throughput, tail latency, memory,
allocation, and error data. Results are comparative release evidence, not
marketing claims.

## Entry Points

Build benchmark targets explicitly:

```bash
xmake f -m debug --build_benchmarks=y
xmake build vio_scheduler_benchmark vio_task_spawn_benchmark vio_timer_heap_benchmark vio_channel_benchmark vio_backend_ping_pong_benchmark
```

Run individual targets with `xmake run`:

```bash
xmake run vio_scheduler_benchmark
xmake run vio_task_spawn_benchmark
xmake run vio_timer_heap_benchmark
xmake run vio_channel_benchmark
xmake run vio_backend_ping_pong_benchmark
```

Current workloads:

- `vio_scheduler_benchmark`: `workload=scheduler_hops`
- `vio_task_spawn_benchmark`: `workload=task_spawn_completed_noop`
- `vio_timer_heap_benchmark`: `workload=timer_heap_add_pop_ready`
- `vio_channel_benchmark`: `workload=bounded_channel_send_receive`
- `vio_backend_ping_pong_benchmark`: `workload=socketpair_ping_pong`

## Record Schema

Each benchmark emits one stable space-separated key=value line per workload or
provider. Required fields are:

```text
benchmark=<name> environment=<platform> platform=<platform> workload=<name> result=<ok|failed|skipped> reason=<machine_reason> operations=<count> elapsed_ns=<count> throughput_ops_per_sec=<count> p50_ns=<count|unknown> p95_ns=<count|unknown> p99_ns=<count|unknown> peak_rss_bytes=<count|unknown> allocations_per_operation=<count|unknown> errors=<count> timeouts=<count>
```

Additional fields may follow the required schema. Current examples include
`backend`, `rounds`, `submitted`, `drained`, `spawned`, `pending`, `sent`,
`received`, `capacity`, `timers_added`, and `timers_ready`.

`environment` and `platform` currently use the same normalized host value:
`linux`, `windows`, `darwin`, or `other`. Keep both fields because downstream
release evidence may later split execution environment from target platform.

Use `unknown` when the benchmark cannot measure a field on the current host or
without adding intrusive instrumentation. Use `0` for counted events that did
not occur. Do not omit required fields.

## Result Semantics

`result=ok` means the benchmark completed its workload and all internal
consistency checks passed.

`result=failed` means the benchmark detected an error, count mismatch, timeout,
or provider failure. Failed records exit non-zero unless a specific benchmark
documents otherwise.

`result=skipped` means the workload is unsupported in the current environment.
Skipped records must use a machine-readable `reason` and exit zero so release
evidence can distinguish unsupported hosts from broken behavior.

## Backend Ping-Pong Evidence

`vio_backend_ping_pong_benchmark` emits one record per backend. On Linux it
attempts epoll first and then io_uring when core capabilities are available.

Example records:

```text
benchmark=backend_ping_pong environment=linux platform=linux workload=socketpair_ping_pong result=ok reason=ok operations=2000 elapsed_ns=... throughput_ops_per_sec=... p50_ns=unknown p95_ns=unknown p99_ns=unknown peak_rss_bytes=... allocations_per_operation=unknown errors=0 timeouts=0 backend=epoll rounds=1000
benchmark=backend_ping_pong environment=linux platform=linux workload=socketpair_ping_pong result=ok reason=ok operations=2000 elapsed_ns=... throughput_ops_per_sec=... p50_ns=unknown p95_ns=unknown p99_ns=unknown peak_rss_bytes=... allocations_per_operation=unknown errors=0 timeouts=0 backend=io_uring rounds=1000
```

The epoll record measures readiness completions plus explicit benchmark-owned
`send()`/`recv()` transfers. The io_uring record measures backend-owned
`write`/`read` transfers and rejects short or zero-byte completions.

On non-Linux hosts, or when io_uring core capabilities are missing, the
benchmark prints `result=skipped` with a machine-readable `reason` and exits 0.
Skipped records are useful environment evidence, but they do not satisfy the
M6-008 io_uring default-enable benchmark gate.

M6-008 consumes benchmark evidence conservatively. A release may satisfy the
benchmark gate only when at least three comparable Linux real-provider runs have
`result=ok` for both epoll and io_uring, matching `workload` and `operations`,
and io_uring median `elapsed_ns / operations` is no worse than 1.10x the epoll
median unless an ADR records a different threshold. Normal tests do not enforce
this performance threshold.

## Release Evidence

Release validation must capture:

- commit hash and dirty/clean status;
- compiler, standard library, build mode, and relevant flags;
- operating system, kernel, CPU model, core count, and power-management notes;
- VXrepo registration and `xrepo info voris-vmem` output;
- exact `xmake f` and `xmake build` commands;
- every benchmark record line, including `result=skipped` records.
