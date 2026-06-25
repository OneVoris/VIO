# ADR 0003 - Timer Heap Versus Timer Wheel

- Status: Proposed
- Date: 2026-06-25
- Owners: repository maintainers
- Related tasks: VIO-M3-009

## Context

M3 introduces timers for sleeps, deadlines, channels, and later backend
timeouts. The default timer data structure must be deterministic, easy to test,
and adequate until benchmark evidence proves it is a bottleneck.

## Decision

Use an indexed binary timer heap as the default timer structure. The heap keeps
timer handles stable through explicit heap-owner ids plus per-heap timer ids.
It supports cancellation by looking up live timer ids in an index map and
eagerly erasing the selected heap entry while repairing the binary-heap
invariant.

Ready extraction has two forms. The unbudgeted form returns every live timer
whose deadline is not later than `now`. The budgeted form consumes one timer
budget unit per distinct ready deadline batch. A consumed unit permits popping
all live timers with that deadline, so timers sharing the same deadline are not
split across scheduler iterations. If the timer budget is already exhausted, the
budgeted form returns no handles and leaves the heap unchanged.

The budgeted form also protects scheduler loops from large forward clock jumps.
When many distinct deadlines are already ready, the heap returns only as many
deadline batches as the current loop budget permits and leaves later ready
deadlines pending for subsequent iterations.

Do not switch to a timer wheel in M3. Timer wheels require bucket sizing,
wraparound rules, clock-jump rules, and workload-specific tuning that are not
justified without benchmark evidence.

## Alternatives Considered

A timer wheel was considered for lower constant factors under very large timer
sets. It was rejected as the M3 default because the repository does not yet have
benchmark evidence showing the heap is a bottleneck.

## Consequences

The heap remains the release default. Future work may introduce a timer wheel
behind the same timer service contract if M8 benchmark records show timer heap
latency, allocation, or CPU cost dominates target workloads.

## Verification

- Unit tests cover same-deadline timers, cancellation, ready extraction, and
  monotonic virtual-clock behavior.
- `benchmarks/timer_heap_benchmark.cpp` records a bounded heap workload that can
  be extended with latency and allocation measurements in M8.
