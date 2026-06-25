# Benchmarks

Benchmarks record environment, workload, throughput, tail latency, memory,
allocation, and system-call data. Results are comparative evidence, not
marketing claims.

Current benchmark entry points:

- `backend_ping_pong_benchmark.cpp`
- `timer_heap_benchmark.cpp`
- `scheduler_benchmark.cpp`
- `channel_benchmark.cpp`

## Backend Ping-Pong Evidence

Build backend benchmarks explicitly:

```bash
xmake f -m debug --build_benchmarks=y
xmake build vio_backend_ping_pong_benchmark
```

`vio_backend_ping_pong_benchmark` emits one stable key=value record per backend:

```text
benchmark=backend_ping_pong environment=linux platform=linux workload=socketpair_ping_pong backend=epoll result=ok reason=ok rounds=1000 operations=2000 elapsed_ns=...
benchmark=backend_ping_pong environment=linux platform=linux workload=socketpair_ping_pong backend=io_uring result=ok reason=ok rounds=1000 operations=2000 elapsed_ns=...
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
