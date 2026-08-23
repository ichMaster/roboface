"""Event validation against ``schema.json``.

Hand-rolled rather than delegated to ``jsonschema``: this runs on the pipeline's
critical path, where a third-party import is a way for the emitter to fail
(architecture §5.2, TRK-001's stdlib-only guarantee).

``validate`` returns a list of human-readable problems rather than raising. The
emitter's contract is to never raise, so a validator that raises would just move the
problem one frame up.
"""

from __future__ import annotations

import json
import re
from functools import lru_cache
from pathlib import Path
from typing import Any

SCHEMA_PATH = Path(__file__).resolve().parent / "schema.json"

#: Current envelope version written by this emitter.
SCHEMA_VERSION = 1


@lru_cache(maxsize=1)
def load() -> dict[str, Any]:
    """The parsed contract. Cached — it is read on every emit."""
    with SCHEMA_PATH.open(encoding="utf-8") as fh:
        result: dict[str, Any] = json.load(fh)
    return result


def event_types() -> list[str]:
    """Every declared event type, sorted."""
    return sorted(load()["events"])


def _scope_family(event_type: str) -> str:
    """Map an event type to its scope family.

    ``harden.finding.fixed`` -> ``harden``; ``issue.validate.end`` -> ``issue``.
    """
    return event_type.split(".", 1)[0]


def required_scope(event_type: str) -> list[str]:
    reqs: dict[str, list[str]] = load()["scope_requirements"]
    return reqs.get(_scope_family(event_type), [])


def validate(event: Any) -> list[str]:
    """Return every problem with ``event``; an empty list means valid."""
    schema = load()
    problems: list[str] = []

    if not isinstance(event, dict):
        return [f"event must be an object, got {type(event).__name__}"]

    env = schema["envelope"]
    allowed = set(env["required"]) | set(env["optional"])

    for key in env["required"]:
        if key not in event:
            problems.append(f"missing required envelope field {key!r}")
    for key in sorted(set(event) - allowed):
        problems.append(f"unknown envelope field {key!r}")

    if "v" in event and event["v"] != SCHEMA_VERSION:
        problems.append(f"unsupported schema version {event['v']!r}")

    for field, pattern in (
        ("ts", env["ts_pattern"]),
        ("run_id", env["run_id_pattern"]),
        ("emitter", env["emitter_pattern"]),
    ):
        value = event.get(field)
        if value is not None and not (isinstance(value, str) and re.match(pattern, value)):
            problems.append(f"{field} {value!r} does not match {pattern}")

    status = event.get("status")
    if status is not None and status not in env["status_values"]:
        problems.append(f"status {status!r} not one of {env['status_values']}")

    event_type = event.get("type")
    if event_type is None:
        return problems
    if event_type not in schema["events"]:
        problems.append(f"unknown event type {event_type!r}")
        return problems

    scope = event.get("scope")
    if not isinstance(scope, dict):
        problems.append("scope must be an object")
    else:
        for key in sorted(set(scope) - set(schema["scope_keys"])):
            problems.append(f"unknown scope key {key!r}")
        for key in required_scope(event_type):
            if not scope.get(key):
                problems.append(f"{event_type} requires scope.{key}")

    data = event.get("data") or {}
    if not isinstance(data, dict):
        problems.append("data must be an object")
    else:
        for key in schema["events"][event_type]["data"]:
            if key not in data:
                problems.append(f"{event_type} requires data.{key}")
        problems.extend(_enum_problems(event_type, data))

    return problems


def _enum_problems(event_type: str, data: dict[str, Any]) -> list[str]:
    """Check the few data fields with a closed value set."""
    enums: dict[str, list[str]] = load()["enums"]
    checks = {
        "issue.start": ("size", "size"),
        "finding.raised": ("severity", "severity"),
        "finding.classified": ("disposition", "disposition"),
        "run.estimate": ("source", "estimate_source"),
    }
    if event_type not in checks:
        return []
    field, enum_name = checks[event_type]
    value = data.get(field)
    if value is not None and value not in enums[enum_name]:
        return [f"{event_type} data.{field} {value!r} not one of {enums[enum_name]}"]
    return []
