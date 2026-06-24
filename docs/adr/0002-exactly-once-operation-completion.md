# ADR 0002 - Exactly-Once Operation Completion

- Status: Proposed
- Date: 2026-06-25
- Owners: repository maintainers
- Related tasks: VIO-M1-004

## Context

VIO presents one asynchronous operation contract across cancellation,
deadlines, structured shutdown, and platform backends such as readiness APIs
and completion APIs. The operation layer must prevent double completion,
use-after-free, accidental callback-thread resumption, and stale native-handle
events after close or handle reuse.

The primary invariant is that each asynchronous operation produces at most one
terminal completion and schedules at most one continuation. Cancellation,
deadline expiry, close, shutdown, backend completion, and owner destruction can
all race with one another, so the contract must define a single arbitration and
lifetime model before backend APIs are implemented.

## Decision

Every backend-facing asynchronous operation is represented by an operation
record. The record stores:

- the captured continuation scheduler;
- the awaiting coroutine continuation, when one exists;
- the native handle identity and generation, when the operation is handle-bound;
- the first cancellation reason, when cancellation is requested;
- the terminal completion result, once selected;
- backend-reference state proving whether a backend can still access the
  operation storage.

The operation record is separate from the user coroutine frame. Destroying the
owner, task, handle object, or scope can withdraw observation and request
cancellation, but it does not directly free operation storage while backend
references may still exist.

### State Machine

The operation lifecycle is:

```text
created
  -> submitting
  -> active
  -> terminal_selected
  -> continuation_queued
  -> observed_or_detached
  -> retired
```

`created` means operation storage exists, the continuation scheduler has been
captured, and no backend can reference the operation yet.

`submitting` means the operation is being registered with the backend or native
handle registry. Submit may either install the backend reference and advance to
`active`, or select a terminal submit failure before any backend reference is
published.

`active` means the backend, native handle registry, timer service, or shutdown
coordinator may reference the operation record. All terminal contenders call the
same `try_complete` arbitration path.

`terminal_selected` means exactly one terminal completion has won. The winning
terminal source may be successful backend completion, backend failure, accepted
cancellation, deadline expiry, close, shutdown, submit failure, or owner
destruction. The terminal result is immutable after this transition.

`continuation_queued` means the terminal result has been published and any
observer continuation has been enqueued on the captured scheduler through the
runtime trampoline.

`observed_or_detached` means the continuation has consumed the result, or the
owner was destroyed and no user continuation remains to resume.

`retired` means all owner-side, scheduler-queue, timer, registry, and backend
references are gone. Only this state may free the operation storage.

### Terminal Arbitration

First terminal completion wins. The operation layer uses a single terminal slot
with compare-exchange style arbitration from `none` to a concrete terminal
result. Once this slot is filled, all later terminal attempts are ignored for
user-visible completion, though they may still release backend references or
record diagnostics.

The terminal contenders are:

- `submit`: publish the backend reference and enter `active`, or complete with a
  submit error if no backend reference was published;
- `cancel`: record the first cancellation reason, request backend cancellation
  when a backend may reference the operation, and attempt cancellation
  completion when the operation contract allows cancellation to complete it;
- `backend completion`: complete with the backend result for the matching
  operation token and handle generation;
- `timeout` or `deadline`: record deadline cancellation as the first reason when
  applicable, request backend cancellation, and attempt deadline completion;
- `close`: complete pending handle-bound operations with close semantics,
  invalidate the handle generation, and request backend cancellation;
- `shutdown`: request cancellation for pending work and complete remaining work
  according to the shutdown phase, including abort completion after a configured
  shutdown deadline;
- `owner destruction`: detach the observer, request cancellation with an owner
  destruction reason, and complete or drain the operation without resuming user
  code.

If cancellation wins terminal arbitration, the first cancellation reason is the
one stored in the terminal result. Later cancellation requests, including a
deadline after an explicit cancel or shutdown after close, must not replace that
reason.

Cancellation is a state transition and a backend request. It is not immediate
operation destruction. A cancelled operation can still receive a backend
completion, cancellation acknowledgement, stale readiness event, or timer
callback. Those later events must be used only to release backend references and
must not produce a second user-visible completion.

### Storage Lifetime

Operation storage remains alive until the backend can no longer reference it.
This includes backends that complete synchronously during submit, complete after
cancellation, deliver cancellation acknowledgements, or report stale events
after close.

The implementation must model owner-side references and backend-side references
separately. The owner may be destroyed before the backend releases the operation;
the backend may release before the user continuation observes the result. Both
sides must release before the operation reaches `retired`.

The backend contract for each provider must state when backend references are
acquired and released. For completion APIs, the operation remains live until the
completion entry or cancellation acknowledgement can no longer contain the
operation token. For readiness APIs, the operation remains live until the
registry has removed the operation from all readiness queues and any in-flight
poll event has been classified as current or stale.

### Continuation Scheduling

Continuations resume only on the scheduler captured for the operation observer.
The terminal winner publishes the result, releases any internal operation lock,
and enqueues the continuation through the runtime trampoline. Backend callbacks,
timer callbacks, close paths, shutdown paths, and cancellation callbacks never
directly resume user coroutine code.

No user code may run while a backend callback is on the stack or while an
internal mutex, registry lock, scheduler queue lock, or operation state lock is
held. Synchronous backend completion still goes through the same trampoline path
so recursive completion chains remain bounded.

### Close, Handle Reuse, and Stale Events

Every native handle observed by the operation layer has a stable registry entry
and a generation. Handle-bound operations store both the handle identity and the
generation active at submit time.

Close invalidates the current generation, selects close completion for pending
operations when it wins arbitration, requests backend cancellation or
deregistration, and prevents new operations from binding to the closed
generation. A later operating-system reuse of the same numeric handle must create
or observe a different generation.

Backend events are accepted only when their operation token and handle
generation match an active operation. Events for old generations, already
retired tokens, or handles that have been closed and reused are stale. Stale
events may release backend bookkeeping, but they must not complete a new
operation and must not overwrite the terminal result of the old operation.

### Atomic Ordering

The operation implementation may use `std::memory_order_seq_cst` by default. Any
non-`seq_cst` atomic operation used in operation arbitration, reference release,
backend-token publication, cancellation reason publication, or continuation
queueing must have a local code comment explaining the happens-before
relationship it relies on.

The comment must identify the releasing operation, the acquiring operation, and
the state protected by that ordering. Unexplained `relaxed`, `acquire`,
`release`, or `acq_rel` orderings are not allowed in operation state-machine
code.

## Alternatives Considered

Inline completion from backend callbacks was rejected because it can resume user
code on provider threads, under backend locks, or in close and cancellation
paths that still own internal invariants.

Destroying operation storage immediately on cancellation was rejected because
backends may still deliver completion, cancellation acknowledgement, or stale
readiness events after cancellation wins user-visible completion.

Treating close as a special path outside terminal arbitration was rejected
because close races with backend completion, cancellation, deadlines, shutdown,
and native handle reuse. Close must participate in the same first-winner rule.

Using only numeric native handles for event matching was rejected because
operating systems can reuse handle values. Generation-aware registry matching is
required to classify stale events safely.

## Consequences

The operation layer has one completion rule for all asynchronous providers:
first terminal completion wins, and every later event is cleanup-only. This
keeps user-visible behavior deterministic even when backend callbacks arrive
after cancellation, close, shutdown, or owner destruction.

Operation records need explicit lifetime accounting beyond coroutine-frame
ownership. This adds implementation complexity, but it is required to avoid
use-after-free and stale-event bugs.

Backends must document their reference-release points and must satisfy the same
contract tests. Provider-specific cancellation limitations are allowed only when
the backend contract states when cancellation can still win and when an already
committed backend result must be reported instead.

The trampoline and captured-scheduler rule make completion behavior predictable,
but they require tests for synchronous completion paths and for callback or
internal-lock paths that would otherwise be tempting to resume inline.

## Verification

Required verification for VIO-M1 and backend implementations:

- submit versus cancel races where cancellation happens before submit, during
  submit, immediately after backend reference publication, and after active
  completion;
- submit failure versus cancellation races proving exactly one terminal result;
- backend success or failure versus cancel races in both arrival orders;
- backend completion versus timeout or deadline races in both arrival orders;
- explicit cancel versus deadline versus shutdown races proving the first
  cancellation reason is retained when cancellation wins;
- close versus backend completion races, including close before submit finishes,
  close while active, close after terminal selection, and close during shutdown;
- native handle reuse tests proving stale events from an old generation cannot
  complete a new operation;
- owner destruction versus backend completion and cancellation acknowledgement
  tests proving operation storage remains alive until backend release;
- shutdown drain and shutdown-deadline abort tests covering pending submitted
  work, cancelled work, completed-but-unobserved work, and detached owners;
- synchronous backend completion tests proving continuations resume through the
  captured scheduler and trampoline rather than inline;
- internal-lock and backend-callback tests proving no user continuation runs
  while private locks are held or while a backend callback is on the stack;
- backend contract tests for deterministic, readiness, and completion backends
  that assert reference acquisition, reference release, cancellation
  acknowledgement, stale event classification, and exactly-once terminal
  arbitration;
- code review or static checks for operation state-machine files proving every
  non-`seq_cst` atomic operation has a local happens-before comment.
