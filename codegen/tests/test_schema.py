"""TRK-002 — the event contract.

The load-bearing test is the last one: the schema and the architecture document's
prose table must agree. They are two representations of one contract, and a contract
that drifts from its documentation is worse than an undocumented one.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import pytest

from tracker import schema

ARCHITECTURE = schema.SCHEMA_PATH.parent.parent / "architecture.md"

#: A minimal valid envelope; tests vary one thing at a time from here.
BASE: dict[str, Any] = {
    "v": 1,
    "ts": "2026-08-03T14:22:31.482Z",
    "run_id": "run-20260803-142012",
    "type": "phase.start",
    "emitter": "skill:ship-phase",
    "scope": {"phase": "v01"},
}

#: Plausible values for every required data key, so an example can be built per type.
SAMPLE: dict[str, Any] = {
    "command": "/ship-phase v01",
    "plan": ["v01.01"],
    "baseline": {"tests": 0, "mypy_errors": 0},
    "git": {"branch": "codegen-tracking", "head_sha": "abc1234", "remote": "origin"},
    "source": "estimated",
    "versions": [{"id": "v01.01", "issues_low": 3, "issues_high": 7}],
    "total": {"issues_low": 3, "issues_high": 7},
    "gap_s": 42,
    "versions_done": 1,
    "issues_done": 5,
    "reason": "superseded",
    "issues": [{"id": "RF-001", "size": "M"}],
    "tag": "v01.01.00",
    "gate": "release",
    "size": "M",
    "area": "games",
    "issue": "RF-001",
    "gh_number": 7,
    "url": "https://example.invalid/7",
    "files_changed": 3,
    "attempt": 1,
    "pytest": {"passed": 41, "failed": 0, "duration_s": 6.1},
    "mypy": {"errors": 0},
    "sha": "abc1234",
    "files": ["games/tictactoe.py"],
    "attempts": 1,
    "finding": "F1",
    "severity": "HIGH",
    "title": "race at await",
    "disposition": "fix-now",
    "home": "v05.01",
    "remote": "origin",
    "tool": "Bash",
}


#: One valid value per scope key.
SCOPE_VALUES = {
    "phase": "v01",
    "version": "v01.01",
    "step": "execute-issues",
    "issue": "RF-001",
}


def _example(event_type: str) -> dict[str, Any]:
    """A valid event of the given type, built from SAMPLE and SCOPE_VALUES."""
    event = dict(BASE, type=event_type)
    event["scope"] = {key: SCOPE_VALUES[key] for key in schema.required_scope(event_type)}
    required = schema.load()["events"][event_type]["data"]
    if required:
        event["data"] = {key: SAMPLE[key] for key in required}
    return event


@pytest.mark.parametrize("event_type", schema.event_types())
def test_a_valid_example_exists_for_every_type(event_type: str) -> None:
    assert schema.validate(_example(event_type)) == []


@pytest.mark.parametrize("event_type", schema.event_types())
def test_each_required_data_key_is_enforced(event_type: str) -> None:
    """Dropping any one required key must fail, and must name that key."""
    required = schema.load()["events"][event_type]["data"]
    for key in required:
        broken = _example(event_type)
        del broken["data"][key]
        problems = schema.validate(broken)
        assert any(f"data.{key}" in p for p in problems), (event_type, key, problems)


@pytest.mark.parametrize("event_type", schema.event_types())
def test_each_required_scope_key_is_enforced(event_type: str) -> None:
    for key in schema.required_scope(event_type):
        broken = _example(event_type)
        del broken["scope"][key]
        problems = schema.validate(broken)
        assert any(f"scope.{key}" in p for p in problems), (event_type, key, problems)


def test_issue_events_require_a_version() -> None:
    """The example from architecture §2.3, pinned."""
    broken = _example("issue.start")
    del broken["scope"]["version"]
    assert any("scope.version" in p for p in schema.validate(broken))


@pytest.mark.parametrize(
    "field,value",
    [
        ("ts", "2026-08-03 14:22:31"),
        ("ts", "2026-08-03T14:22:31Z"),
        ("run_id", "run-2026-08-03"),
        ("emitter", "ship-phase"),
        ("emitter", "Skill:Ship-Phase"),
    ],
)
def test_malformed_envelope_fields_are_rejected(field: str, value: str) -> None:
    assert any(field in p for p in schema.validate(dict(BASE, **{field: value})))


def test_unknown_event_type_is_rejected() -> None:
    assert any("unknown event type" in p for p in schema.validate(dict(BASE, type="nope.nope")))


def test_unknown_envelope_and_scope_fields_are_rejected() -> None:
    assert any("unknown envelope field" in p for p in schema.validate(dict(BASE, extra=1)))
    bad_scope = dict(BASE, scope={"phase": "v01", "nope": "x"})
    assert any("unknown scope key" in p for p in schema.validate(bad_scope))


def test_closed_enums_are_enforced() -> None:
    bad = _example("finding.raised")
    bad["data"]["severity"] = "CRITICAL"
    assert any("severity" in p for p in schema.validate(bad))


def test_non_dict_input_is_reported_not_raised() -> None:
    values: list[Any] = ["string", 42, None, []]
    for value in values:
        assert schema.validate(value)


def test_schema_matches_the_architecture_documents_table() -> None:
    """The prose table in architecture §3 and schema.json are one contract.

    Drift here is how an implementation ends up satisfying a document nobody can
    trust, so it fails the suite rather than being noticed later.
    """
    text = Path(ARCHITECTURE).read_text(encoding="utf-8")
    table = text.split("## 3. Event catalogue")[1].split("### 3.1")[0]
    documented = set(re.findall(r"^\| `([a-z][a-z.]+)`", table, re.MULTILINE))
    assert documented == set(schema.event_types()), {
        "only in architecture.md": sorted(documented - set(schema.event_types())),
        "only in schema.json": sorted(set(schema.event_types()) - documented),
    }
