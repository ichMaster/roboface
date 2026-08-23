"""Append one event to the active run's log. The only writer.

The load-bearing property is that :func:`emit` **never raises** (architecture §5.2).
Tracking observes the pipeline; it must never be able to fail it. Every failure mode —
unwritable path, missing directory, full disk, non-serialisable payload, absent run —
is swallowed and best-effort recorded to ``codegen/var/emit-errors.log``.

Append discipline (architecture §4.2), which makes concurrent writers safe:

* ``O_WRONLY | O_APPEND | O_CREAT``, never a seek;
* the whole line, newline included, in **exactly one** ``os.write()``;
* the line budgeted to 4096 bytes, truncating the payload rather than splitting.
"""

from __future__ import annotations

import json
import os
import sys
from datetime import UTC, datetime
from typing import Any

from tracker import paths, redact, schema

#: Byte budget for one line, including its newline (architecture §4.2).
MAX_LINE_BYTES = 4096

#: Where emit failures go. Best effort — if this fails too, we give up silently.
ERROR_LOG_NAME = "emit-errors.log"


def now_iso() -> str:
    """Current UTC time in the envelope's format: millisecond precision, ``Z``."""
    now = datetime.now(UTC)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


def current_run_id() -> str | None:
    """The active run id, or ``None`` when there is nothing to attribute to."""
    try:
        pointer = paths.current_pointer()
        if not pointer.is_file():
            return None
        run_id = pointer.read_text(encoding="utf-8").strip()
        return run_id or None
    except OSError:
        return None


def build(
    event_type: str,
    *,
    emitter: str,
    scope: dict[str, str] | None = None,
    status: str | None = None,
    data: dict[str, Any] | None = None,
    run_id: str | None = None,
    ts: str | None = None,
) -> dict[str, Any]:
    """Assemble a redacted, budget-fitted event. Pure — does no I/O beyond the clock."""
    event: dict[str, Any] = {
        "v": schema.SCHEMA_VERSION,
        "ts": ts or now_iso(),
        "run_id": run_id or "",
        "type": event_type,
        "emitter": emitter,
        "scope": dict(scope or {}),
    }
    if status is not None:
        event["status"] = status
    if data:
        event["data"] = data

    event = redact.redact(event)
    return _fit_to_budget(event)


def _fit_to_budget(event: dict[str, Any]) -> dict[str, Any]:
    """Shrink ``data`` until the serialised line fits, never splitting the line."""
    if _line_bytes(event) <= MAX_LINE_BYTES:
        return event
    data = event.get("data")
    if isinstance(data, dict):
        for limit in (512, 128, 32):
            trimmed, hit = redact.truncate(data, limit)
            candidate = dict(event, data={**trimmed, "_truncated": True} if hit else trimmed)
            if _line_bytes(candidate) <= MAX_LINE_BYTES:
                return candidate
        # Payload is pathological: keep the envelope, drop the body. Never drop the event.
        return dict(event, data={"_truncated": True, "_dropped": True})
    return event


def _line_bytes(event: dict[str, Any]) -> int:
    return len(_serialise(event))


def _serialise(event: dict[str, Any]) -> bytes:
    line = json.dumps(event, separators=(",", ":"), ensure_ascii=False, default=str)
    return (line + "\n").encode("utf-8")


def emit(
    event_type: str,
    *,
    emitter: str,
    scope: dict[str, str] | None = None,
    status: str | None = None,
    data: dict[str, Any] | None = None,
) -> None:
    """Append one event. Never raises, never blocks beyond a single write."""
    try:
        run_id = current_run_id()
        if run_id is None:
            return
        event = build(
            event_type, emitter=emitter, scope=scope, status=status, data=data, run_id=run_id
        )
        problems = schema.validate(event)
        if problems:
            _record_failure(f"invalid {event_type}: {'; '.join(problems)}")
            return
        append_line(paths.events_path(run_id), _serialise(event))
    except BaseException as exc:  # noqa: BLE001 - the never-raise guarantee is the point
        _record_failure(f"{event_type}: {type(exc).__name__}: {exc}")


def append_line(path: Any, payload: bytes) -> None:
    """One ``open`` + one ``write`` + one ``close``. Raises; callers must guard."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        os.write(fd, payload)
    finally:
        os.close(fd)


def _record_failure(message: str) -> None:
    """Best-effort note that an emit was lost. Silence here is acceptable; a raise is not."""
    try:
        var = paths.var_root()
        var.mkdir(parents=True, exist_ok=True)
        with (var / ERROR_LOG_NAME).open("a", encoding="utf-8") as fh:
            fh.write(f"{now_iso()} {message}\n")
    except BaseException:  # noqa: BLE001, S110 - nothing left to do
        pass


def _main(argv: list[str]) -> int:
    """CLI for hooks and shell callers: ``python3 -m tracker.emit <type> [...]``."""
    import argparse

    parser = argparse.ArgumentParser(prog="tracker.emit", description="Append one tracking event.")
    parser.add_argument("type")
    parser.add_argument("--emitter", required=True)
    parser.add_argument("--scope", default="", help="comma-separated key=value pairs")
    parser.add_argument("--status", default=None)
    parser.add_argument("--data", default=None, help="JSON object")
    args = parser.parse_args(argv)

    scope = dict(
        pair.split("=", 1) for pair in args.scope.split(",") if "=" in pair
    )
    try:
        data = json.loads(args.data) if args.data else None
    except json.JSONDecodeError as exc:
        _record_failure(f"CLI --data not JSON: {exc}")
        return 0  # never non-zero: a hook must not disturb the session it observes
    emit(args.type, emitter=args.emitter, scope=scope, status=args.status, data=data)
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised via subprocess in tests
    sys.exit(_main(sys.argv[1:]))
