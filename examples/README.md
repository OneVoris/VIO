# Examples

These examples are deterministic local demos, not production servers. They are
small programs intended to build quickly and exercise public VIO APIs without
external services or unbounded waits.

## Build

```bash
xmake f -m debug --build_examples=y
xmake build vio_example_echo
xmake build vio_example_fan_out
xmake build vio_example_file_copy
xmake build vio_example_graceful_shutdown
```

## Run

```bash
xmake run vio_example_echo
xmake run vio_example_fan_out
xmake run vio_example_file_copy
xmake run vio_example_graceful_shutdown
```

## Programs

- `echo.cpp` demonstrates a bounded echo-style read/write operation flow with
  the public socket operation queue.
- `fan_out.cpp` installs an explicit scheduler, fans out two tasks with
  `when_all`, and verifies the aggregate result.
- `file_copy.cpp` copies a temporary payload through VIO file open, async
  read, async write, sync, and close operations.
- `graceful_shutdown.cpp` starts a runtime, submits bounded work, requests
  stop, joins, and verifies that accepted work drained while post-stop work is
  rejected.
