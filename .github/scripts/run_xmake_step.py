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


ANSI_PATTERN = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
ERROR_PATTERN = re.compile(
    r"(fatal(?: error)?:|error:|undefined reference|failed)",
    re.IGNORECASE,
)
FILE_DIAGNOSTIC_PREFIX = (
    r"(^|[\s\"'])"
    r"(?:[A-Za-z]:[\\/])?"
    r"[^:\n]+:\d+(?::\d+)?:\s*"
)
TOOL_DIAGNOSTIC_PREFIX = (
    r"(^|[\s\"'])"
    r"(?:collect2|cc1plus|cc1|gcc|g\+\+|clang|clang\+\+):\s*"
)
LINKER_DIAGNOSTIC_PREFIX = (
    r"(^|[\s\"'])"
    r"(?:[A-Za-z]:[\\/])?"
    r"(?:[^\s:\n]+[\\/])*"
    r"ld(?:\.lld|\.gold)?(?:\.exe)?:\s*"
)
PRIMARY_COMPILER_DIAGNOSTIC_PATTERN = re.compile(
    FILE_DIAGNOSTIC_PREFIX
    + r"(?:fatal error|error):"
    + r"|"
    + TOOL_DIAGNOSTIC_PREFIX
    + r"(?:fatal\s+)?error:"
    + r"|"
    + LINKER_DIAGNOSTIC_PREFIX
    + r"(?:cannot find|fatal error:|error:|undefined reference)"
    + r"|undefined reference to",
    re.IGNORECASE,
)
SECONDARY_COMPILER_DIAGNOSTIC_PATTERN = re.compile(
    FILE_DIAGNOSTIC_PREFIX
    + r"(?:warning|note):"
    + r"|"
    + TOOL_DIAGNOSTIC_PREFIX
    + r"(?:warning|note):"
    + r"|"
    + LINKER_DIAGNOSTIC_PREFIX
    + r"(?:warning|note):",
    re.IGNORECASE,
)
TAIL_LINES = 80
ANNOTATION_TAIL_CHARS = 12000
MAX_DIAGNOSTIC_CANDIDATES = 8
DIAGNOSTIC_CONTEXT_BEFORE = 12
DIAGNOSTIC_CONTEXT_AFTER = 10
DIAGNOSTIC_CONTEXT_CHARS = 8000


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


def strip_ansi(value: str) -> str:
    return ANSI_PATTERN.sub("", value)


def is_xmake_stack_line(line: str) -> bool:
    stripped = line.strip()
    lowered = stripped.lower()
    if not stripped:
        return False
    if lowered in {"stack traceback:", "traceback:"}:
        return True
    if lowered.startswith("stack traceback:") or lowered.startswith("traceback:"):
        return True
    if re.match(r"^\[c\]:\s+in\s+", lowered):
        return True
    if re.search(r"@programdir[\\/].*\.lua:\d+", stripped, re.IGNORECASE):
        return True
    if re.search(r"[\\/]xmake[\\/].*\.lua:\d+", stripped, re.IGNORECASE):
        return True
    if re.search(r"[\\/]xrepo[\\/].*\.lua:\d+", stripped, re.IGNORECASE):
        return True
    return False


def is_primary_compiler_diagnostic_line(line: str) -> bool:
    if is_xmake_stack_line(line):
        return False
    return bool(PRIMARY_COMPILER_DIAGNOSTIC_PATTERN.search(line))


def is_secondary_compiler_diagnostic_line(line: str) -> bool:
    if is_xmake_stack_line(line):
        return False
    return bool(SECONDARY_COMPILER_DIAGNOSTIC_PATTERN.search(line))


def is_compiler_diagnostic_line(line: str) -> bool:
    return is_primary_compiler_diagnostic_line(
        line
    ) or is_secondary_compiler_diagnostic_line(line)


def sanitized_log_lines(log_file: Path) -> list[str]:
    try:
        text = log_file.read_bytes().decode(errors="replace")
    except OSError:
        return []
    return [strip_ansi(line).rstrip() for line in text.splitlines()]


def trim_block(lines: list[str], limit: int = DIAGNOSTIC_CONTEXT_CHARS) -> str:
    block = "\n".join(lines).strip()
    if len(block) <= limit:
        return block
    return block[: limit - 20].rstrip() + "\n... [truncated]"


def collect_diagnostic_candidates(
    lines: list[str],
    predicate,
    limit: int,
) -> list[str]:
    candidates: list[str] = []
    seen: set[str] = set()
    for line in lines:
        clean = line.strip()
        if not clean or not predicate(clean):
            continue
        if clean in seen:
            continue
        candidates.append(trim_line(clean))
        seen.add(clean)
        if len(candidates) >= limit:
            break
    return candidates


def compiler_diagnostic_candidates(lines: list[str]) -> list[str]:
    primary = collect_diagnostic_candidates(
        lines,
        is_primary_compiler_diagnostic_line,
        MAX_DIAGNOSTIC_CANDIDATES,
    )
    if primary:
        return primary
    return collect_diagnostic_candidates(
        lines,
        is_secondary_compiler_diagnostic_line,
        MAX_DIAGNOSTIC_CANDIDATES,
    )


def first_compiler_diagnostic_context(lines: list[str]) -> str | None:
    diagnostic_index = next(
        (
            index
            for index, line in enumerate(lines)
            if is_primary_compiler_diagnostic_line(line)
        ),
        None,
    )
    if diagnostic_index is None:
        diagnostic_index = next(
            (
                index
                for index, line in enumerate(lines)
                if is_secondary_compiler_diagnostic_line(line)
            ),
            None,
        )
    if diagnostic_index is None:
        return None

    start = max(0, diagnostic_index - DIAGNOSTIC_CONTEXT_BEFORE)
    end = min(len(lines), diagnostic_index + DIAGNOSTIC_CONTEXT_AFTER + 1)
    context: list[str] = []
    for line in lines[start:end]:
        if is_xmake_stack_line(line):
            continue
        context.append(line)
    return trim_block(context)


def first_relevant_log_line(lines: list[str]) -> str | None:
    for line in lines:
        clean = line.strip()
        if clean and not is_xmake_stack_line(clean) and ERROR_PATTERN.search(clean):
            return clean
    return None


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

    sanitized_lines = sanitized_log_lines(log_file)
    candidates = compiler_diagnostic_candidates(sanitized_lines)
    diagnostic_context = first_compiler_diagnostic_context(sanitized_lines)
    first_relevant_line = first_relevant_log_line(sanitized_lines) or first_error_line

    message_parts = [
        f"Command: {command_text(command)}",
        f"Exit code: {exit_code}",
        f"Log file: {log_file}",
    ]
    if first_relevant_line:
        message_parts.append(f"First relevant log line: {trim_line(first_relevant_line)}")
    if candidates:
        message_parts.append(
            "Compiler diagnostic candidates:\n"
            + "\n".join(f"- {candidate}" for candidate in candidates)
        )
    if diagnostic_context:
        message_parts.append(f"Compiler diagnostic context:\n{diagnostic_context}")
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
            clean = strip_ansi(text).strip()
            if (
                first_error["line"] is None
                and clean
                and not is_xmake_stack_line(clean)
                and ERROR_PATTERN.search(clean)
            ):
                first_error["line"] = clean


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
            log_file.flush()
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
