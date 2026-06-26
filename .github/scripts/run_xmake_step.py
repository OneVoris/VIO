#!/usr/bin/env python3
"""Run an xmake-related command and emit useful GitHub annotations on failure."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import os
import re
import shlex
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


ERROR_PATTERN = re.compile(
    r"(fatal(?: error)?:|error:|undefined reference|failed)",
    re.IGNORECASE,
)
TAIL_LINES = 80
ANNOTATION_TAIL_CHARS = 12000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a command, tee output to a log, and annotate failures."
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to run. Use '--' before the command when it has options.",
    )
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("missing command after '--'")
    return args


def command_text(command: list[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def log_path() -> Path:
    root = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir()))
    root.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return root / f"vio-xmake-step-{stamp}-{os.getpid()}.log"


def escape_command_value(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_property_value(value: str) -> str:
    return (
        escape_command_value(value)
        .replace(":", "%3A")
        .replace(",", "%2C")
    )


def trim_line(line: str, limit: int = 600) -> str:
    line = line.strip()
    if len(line) <= limit:
        return line
    return line[: limit - 3] + "..."


def emit_error_annotation(
    *,
    command: list[str],
    exit_code: int,
    log_file: Path,
    first_error_line: str | None,
    tail_lines: collections.deque[str],
) -> None:
    tail = "".join(tail_lines).strip()
    if len(tail) > ANNOTATION_TAIL_CHARS:
        tail = "..." + tail[-ANNOTATION_TAIL_CHARS:]
    if not tail:
        tail = "(no output)"

    message_parts = [
        f"Command: {command_text(command)}",
        f"Exit code: {exit_code}",
        f"Log file: {log_file}",
    ]
    if first_error_line:
        message_parts.append(f"First relevant log line: {trim_line(first_error_line)}")
    message_parts.append(f"Log tail:\n{tail}")

    message = "\n".join(message_parts)
    title = escape_property_value("xmake command failed")
    print(f"::error title={title}::{escape_command_value(message)}", file=sys.stderr)


def tee_stream(
    source,
    target,
    log_file,
    log_lock: threading.Lock,
    tail_lines: collections.deque[str],
    tail_lock: threading.Lock,
    first_error: dict[str, str | None],
) -> None:
    while True:
        chunk = source.readline()
        if not chunk:
            break

        target.buffer.write(chunk)
        target.buffer.flush()

        with log_lock:
            log_file.write(chunk)
            log_file.flush()

        text = chunk.decode(errors="replace")
        with tail_lock:
            tail_lines.append(text)
            if first_error["line"] is None and ERROR_PATTERN.search(text):
                first_error["line"] = text


def run_command(command: list[str]) -> int:
    log_file_path = log_path()
    tail_lines: collections.deque[str] = collections.deque(maxlen=TAIL_LINES)
    first_error: dict[str, str | None] = {"line": None}
    log_lock = threading.Lock()
    tail_lock = threading.Lock()

    with log_file_path.open("wb") as log_file:
        try:
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            line = f"Failed to start command: {exc}\n"
            sys.stderr.write(line)
            sys.stderr.flush()
            log_file.write(line.encode(errors="replace"))
            tail_lines.append(line)
            emit_error_annotation(
                command=command,
                exit_code=127,
                log_file=log_file_path,
                first_error_line=line,
                tail_lines=tail_lines,
            )
            return 127

        assert process.stdout is not None
        assert process.stderr is not None

        stdout_thread = threading.Thread(
            target=tee_stream,
            args=(
                process.stdout,
                sys.stdout,
                log_file,
                log_lock,
                tail_lines,
                tail_lock,
                first_error,
            ),
        )
        stderr_thread = threading.Thread(
            target=tee_stream,
            args=(
                process.stderr,
                sys.stderr,
                log_file,
                log_lock,
                tail_lines,
                tail_lock,
                first_error,
            ),
        )
        stdout_thread.start()
        stderr_thread.start()
        exit_code = process.wait()
        stdout_thread.join()
        stderr_thread.join()

    if exit_code != 0:
        emit_error_annotation(
            command=command,
            exit_code=exit_code,
            log_file=log_file_path,
            first_error_line=first_error["line"],
            tail_lines=tail_lines,
        )
    return exit_code


def main() -> int:
    args = parse_args()
    return run_command(args.command)


if __name__ == "__main__":
    raise SystemExit(main())
