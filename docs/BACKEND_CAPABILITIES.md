# Backend Capabilities, Limits, and Failure Modes

This document is the release-facing capability map for VIO backends and file
I/O paths. It describes stable behavior that callers and release reviewers may
depend on. Provider diagnostics, platform wording, errno names, Windows status
strings, and other diagnostic text is not an API.

## Backend Matrix

| Backend/path | Platforms | Default selection | Primary use | Release evidence status |
|---|---|---|---|---|
| `virtual` | All | Never a production default | Deterministic contract backend for tests, synthetic completions, close/shutdown race checks, and stale-generation behavior. | Must pass the shared backend contract suite in every release validation. It does not prove real provider behavior. |
| `epoll` | Linux | Linux default unless io_uring release gates all pass | Readiness backend for sockets, `eventfd` wake-up, generation-aware file-descriptor registry, and readiness-to-operation completion mapping. | Linux provider evidence is required for release. It remains the conservative Linux default while io_uring default-enable gates are incomplete. |
| `io_uring` | Linux with io_uring support | Optional; selected only when capability and evidence gates pass | Completion backend for socket read, socket write, accept, connect, file read, file write, and fsync. Registered buffers and files are optional optimizations. | M6 tests cover capability detection, deterministic kernel behavior, differential checks, and real Linux paths when available. It is not the default unless all default-enable gates are recorded. |
| `kqueue` | macOS and BSD systems with kqueue | Platform backend on kqueue platforms | Readiness backend for sockets, `EVFILT_USER` wake-up, generation-aware descriptor cookies, and stale-event filtering. | M7 implementation tests exist. M7-004 macOS/BSD release contract evidence is still a known gap. On non-kqueue platforms operations return `unsupported`. |
| `IOCP` | Windows | Platform backend on Windows | Completion-port skeleton with IOCP port ownership, handle association, bounded completion keys/batches, heap-stable internal `OVERLAPPED` storage, native packet mapping/cleanup, and deterministic synthetic/test completion paths. | M7 implementation tests exist. M7-003 has not yet wired real `ReadFile`/`WriteFile` submission, and this row must not be read as a fully wired real socket or file provider. See `docs/PLATFORM_BACKENDS.md` for current native-submission limits. M7-004 Windows release contract evidence is still a known gap. |
| `file/blocking-executor` | All platforms supported by `std::filesystem` and native file streams | Portable file path until a platform file provider is explicitly used | File open, close, file read, file write, size, truncate, allocation hint, `sync_data`, `sync_all`, and sendfile-compatible borrowed views through an explicit bounded `blocking_executor`. | M5 file tests cover queue saturation, short progress, close, and disk errors. It is not an event backend and does not register handles, poll, or wake. |

The repository may provide default scheduler types, but VIO does not install a
hidden fallback scheduler. Users, examples, and tests must explicitly create,
install, or pass the scheduler/executor that owns continuations and blocking
file work.

## Capability Ownership

| Capability | `virtual` | `epoll` | `io_uring` | `kqueue` | `IOCP` | `file/blocking-executor` |
|---|---|---|---|---|---|---|
| Backend lifecycle: register, submit, cancel, close, poll, drain, wake, shutdown | Yes, deterministic in-memory behavior. | Yes on Linux; non-Linux reports `unsupported`. | Yes when capabilities are available; falls back or reports `unsupported` when unavailable. | Yes on kqueue platforms; non-kqueue platforms report `unsupported`. | Yes on Windows; non-Windows reports `unsupported`. | N/A. File work is submitted to the explicit executor and resumes on the captured scheduler. |
| Socket read | Synthetic readiness completion. | Readiness signal; nonblocking helper performs the syscall and may return `operation_in_progress`. | Kernel completion when read support is present. | Readiness signal; platform socket helper semantics are backend-neutral. | Operation storage, association keys, and `OVERLAPPED` lifetime are modeled; real socket read submission is not release-documented as wired yet. Synthetic/test packets exercise mapping and cleanup. | N/A. |
| Socket write | Synthetic readiness completion. | Readiness signal; nonblocking helper performs the syscall and may return `operation_in_progress`. | Kernel completion when write support is present. | Readiness signal; platform socket helper semantics are backend-neutral. | Operation storage, association keys, and `OVERLAPPED` lifetime are modeled; real socket write submission is not release-documented as wired yet. Synthetic/test packets exercise mapping and cleanup. | N/A. |
| Accept | Synthetic readiness completion. | Readiness signal; Linux `accept4` helper maps retryable pending cases to `operation_in_progress`. | Kernel completion when accept support is present. | Readiness signal. | Operation storage and native-packet mapping are modeled; real accept submission is not release-documented as wired yet. Synthetic/test packets exercise cleanup-only stale handling. | N/A. |
| Connect | Synthetic readiness completion. | Readiness signal; Linux `connect`/`SO_ERROR` helper maps pending connect to `operation_in_progress`. | Kernel completion when connect support is present. | Readiness signal. | Operation storage and native-packet mapping are modeled; real connect submission is not release-documented as wired yet. Synthetic/test packets exercise cleanup-only stale handling. | N/A. |
| File read | Synthetic file operation payloads for tests. | Rejected as `invalid_state`; epoll owns socket readiness only. | Kernel completion when file support is present. | Rejected as `invalid_state`; kqueue backend owns socket readiness only. | Operation storage and `OVERLAPPED` lifetime are modeled; M7-003 has not yet wired real `ReadFile`/`WriteFile` submission. Synthetic/test completions exercise deterministic completion paths. | Public portable async file reads run through the bounded blocking executor. |
| File write | Synthetic file operation payloads for tests. | Rejected as `invalid_state`; epoll owns socket readiness only. | Kernel completion when file support is present. | Rejected as `invalid_state`; kqueue backend owns socket readiness only. | Operation storage and `OVERLAPPED` lifetime are modeled; M7-003 has not yet wired real `ReadFile`/`WriteFile` submission. Synthetic/test completions exercise deterministic completion paths. | Public portable async file writes run through the bounded blocking executor. |
| fsync | Synthetic file operation payloads for tests. | Rejected as `invalid_state`. | Kernel completion when fsync support is present. | Rejected as `invalid_state`. | Operation storage and cleanup paths are modeled; real Windows fsync provider submission is not release-documented as wired yet. Synthetic/test completions cover deterministic completion mapping. | `sync_data` and `sync_all` flush through the portable file path. |
| truncate | N/A backend operation. | N/A backend operation. | N/A backend operation in M8. | N/A backend operation. | N/A backend operation. | Owned by the portable file path. |
| allocation hint | N/A backend operation. | N/A backend operation. | N/A backend operation in M8. | N/A backend operation. | N/A backend operation. | Owned by the portable file path; current implementation grows the file when needed. |

Backends never resume user coroutine code inline from provider callbacks,
internal locks, cancellation paths, close paths, or shutdown paths. Visible
completion is published through the exactly-once operation state machine and the
captured continuation scheduler.

## Limits and Backpressure

- Bounded queues are a release invariant. Shard queues, mailboxes, channels,
  wait queues, executors, and provider queues either have explicit capacities or
  derive strict limits from an owner.
- Mailboxes have separate user and system queues derived from runtime options.
  Full queues return `resource_exhausted` and record saturation pressure.
- Operation slots are bounded by the owning structure. Duplicate operation ids
  are `invalid_state`, active ids stay reserved until completion is drained, and
  IOCP association slots are capped by `association_capacity`.
- Provider batches are capped. io_uring has `submission_queue_capacity`,
  `submit_batch_limit`, and `completion_batch_limit`; IOCP has
  `completion_batch_limit` and `native_packet_capacity`; readiness providers
  drain a bounded native event batch per poll.
- The blocking executor queue is explicit and bounded. A full queue returns
  `resource_exhausted`; shutdown rejects later work with `closed`.
- The same-direction socket op queue is FIFO and bounded separately for read
  direction and write direction. Overflow returns `resource_exhausted`.
- The M8 memory ceiling proxy is owner-derived hard capacity, not an RSS hard
  limit. Release evidence must show rejected work does not run later and does
  not leave unbounded queue growth behind.
- `virtual_backend` keeps in-memory pending/completion vectors for contract
  tests. It is not a production traffic backpressure model.

## Failure Modes and Stable Error Mapping

VIO exposes stable `vio_error_code` classifications. Platform provider errors
may be preserved as `provider_code`, but provider-specific diagnostics are for
logs and troubleshooting only; diagnostic text is not an API.

| Error code | Stable meaning |
|---|---|
| `invalid_state` | The operation shape, scheduler state, handle token, generation, operation id, registration state, or option set is invalid for the requested action. Missing explicit scheduler installation is also `invalid_state`. |
| `resource_exhausted` | A bounded queue, mailbox, operation slot set, provider submission queue, IOCP association table, native packet queue, same-direction socket op queue, or blocking executor queue is full. |
| `operation_in_progress` | A nonblocking socket helper reached a provider pending state such as `EAGAIN`, `EWOULDBLOCK`, `EINPROGRESS`, or an equivalent connect/accept/read/write retry condition. |
| `closed` | Work was submitted after close or shutdown, or close/shutdown won terminal arbitration for pending work. |
| `cancelled` | Cancellation won terminal arbitration. The first cancellation reason is retained and later cancel/deadline/shutdown reasons do not rewrite it. |
| `deadline_exceeded` | A deadline or timeout won terminal arbitration or triggered cancellation according to the owning operation contract. |
| `unsupported` | The backend, provider feature, or native helper is unavailable on the current platform or kernel. |
| `backend_failure` | The provider or filesystem reported a real failure that is not represented by a more specific stable VIO classification. |

Provider errors are normalized before they cross the public boundary. For
example, Linux errno values and Windows completion statuses can produce
`provider_code`, but callers must branch on the stable classification, not on
diagnostic text.

## Cancellation, Close, and Shutdown

Cancellation follows ADR 0002: cancel is a request and a state transition, not
operation destruction. A backend may still deliver a completion, cancellation
acknowledgement, stale readiness event, or provider packet after cancellation
wins user-visible arbitration. Later events are cleanup-only.

Portable file I/O has an unavoidable limitation: a started blocking file syscall may complete normally.
If a blocking read, write, truncate, allocation hint, or sync operation has
already entered the provider, VIO reports the real provider result rather than
masking a completed side effect as `cancelled`.

Close invalidates the current native handle generation. Backends must defend
against native handle reuse and stale events by matching both operation token
and generation before producing a visible completion. Stale provider events may
release backend bookkeeping but must not complete a new operation or rewrite an
old terminal result.

Shutdown rejects new work with `closed`, requests cancellation or provider
drain for active work, and keeps operation storage alive until backend
references are gone. Already queued visible completions remain observable until
drained according to the backend contract.

IOCP owns heap-stable internal `OVERLAPPED` storage for submitted operations.
Storage remains active until the matching completion packet, cancellation
packet, close completion, shutdown completion, or cleanup-only stale packet has
been observed and drained. `CancelIoEx` is a request; the original native
completion can still arrive later. Current release docs cover IOCP storage,
association, packet mapping, and deterministic synthetic/test completion paths;
they do not claim real socket/file provider submission is fully wired.

io_uring default-enable gates are conservative. `default_eligible()` only
reflects the core capability set for an already constructed backend. Linux
default selection still requires recorded cancellation-race, differential,
benchmark, and real-provider evidence. Missing gates keep Linux on epoll.

kqueue behavior is platform-limited. On non-kqueue platforms the backend keeps
returning `unsupported` for register, submit, cancel, close, poll, drain, wake,
and shutdown instead of silently emulating kqueue semantics.

## Known Evidence Gaps

- VIO-M7-004 is not complete: the full backend contract suite still needs real
  macOS/BSD and Windows evidence for kqueue and IOCP before the global backend
  contract DoD can be checked.
- Global release DoD still requires complete Debug, Release, ASan+UBSan, and
  TSan evidence for the release candidate. Existing jobs and documentation are
  gates, not proof that every configuration has passed for a future tag.
- The repository records the resolved `voris-vmem` version from
  `xrepo info voris-vmem` as release evidence. The repository does not pin a
  VMem version selector in `xmake.lua`.
- io_uring remains non-default until every default-enable evidence gate is
  recorded for the release kernel/provider.
- Provider-specific diagnostics remain best-effort. Only stable VIO error codes
  and documented ownership/lifetime rules are compatibility promises.

## Release Checklist

Before claiming a backend-ready release:

1. Record commit hash, clean/dirty status, operating system/kernel, compiler,
   standard library, build flags, and CPU.
2. Record VXrepo registration and the actual `xrepo info voris-vmem` resolution
   used for validation. The repository does not pin `voris-vmem`; do not add a
   version selector to `add_requires("voris-vmem")`.
3. Run repository validation, Debug and Release builds, `xmake test`, required
   sanitizer jobs, hardening stress, differential/backend contract suites,
   interop/crash evidence when applicable, and required benchmarks.
4. Record pass/skip/fail output for unsupported-provider paths. A skip is
   acceptable only when the backend is unsupported on the host and the skip path
   itself is tested.
5. Keep VIO-M7-004 and global DoD items unchecked until the exact release
   evidence proves them.
