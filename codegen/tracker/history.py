"""Cross-run history — one run is an anecdote, several are data.

Vision §6.3. This is where the project's actual question gets answered: **is generation
reliably wrong in the same places?** A single run cannot say; the same phase generated
five times can.

The index rebuilds from the run directories alone, so deleting it loses nothing.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime
from typing import Any

from tracker import paths
from tracker.reduce import reduce

#: Sentinel distinguishing "this version was never run" from "it ran with zero
#: failures". They are different claims, and a heatmap that conflates them lies.
NOT_RUN = None


def summarise(run_id: str, now: datetime | None = None) -> dict[str, Any]:
    """One row of the index, reduced from that run's log."""
    events = paths.events_path(run_id)
    lines = events.read_text(encoding="utf-8").splitlines() if events.is_file() else []
    state = reduce(lines, now or datetime.now(UTC))

    per_version: dict[str, dict[str, Any]] = {}
    for phase in state.tree:
        for version in phase.get("children") or []:
            issues = [
                issue
                for step in (version.get("children") or [])
                for issue in (step.get("children") or [])
            ]
            failures = sum(
                max(0, int((issue.get("data") or {}).get("attempts", 1)) - 1) for issue in issues
            )
            per_version[str(version["id"])] = {
                "duration_s": version.get("elapsed_s", 0),
                "issues": len(issues),
                "failures": failures,
                "status": version.get("status"),
            }

    return {
        "run_id": run_id,
        "command": state.command,
        "status": state.status,
        "started": state.started,
        "elapsed_s": state.elapsed_s,
        "idle_s": state.idle_s,
        "first_pass_rate": state.metrics.get("first_pass_rate"),
        "issues_done": state.metrics.get("issues_done", 0),
        "tests_passing": state.metrics.get("tests_passing", 0),
        "branch": state.github.get("branch"),
        "versions": per_version,
    }


def build_index(now: datetime | None = None) -> dict[str, Any]:
    """Summarise every run directory. Rebuildable; never the source of truth."""
    runs = sorted(p.name for p in paths.runs_root().glob("run-*") if p.is_dir())
    return {
        "generated_from": "codegen/runs/",
        "runs": [summarise(run_id, now) for run_id in runs],
    }


def write_index(now: datetime | None = None) -> dict[str, Any]:
    index = build_index(now)
    target = paths.runs_root() / "index.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    return index


def failure_heatmap(index: dict[str, Any]) -> dict[str, dict[str, int | None]]:
    """version -> run_id -> failures, with ``None`` where the version did not run.

    The distinction is the point: a cell that is dark in *every* run is a
    specification problem, not a model problem — but only if "did not run" cannot be
    mistaken for "ran cleanly".
    """
    runs = index["runs"]
    versions = sorted({v for run in runs for v in run["versions"]})
    return {
        version: {
            run["run_id"]: (
                run["versions"][version]["failures"] if version in run["versions"] else NOT_RUN
            )
            for run in runs
        }
        for version in versions
    }


def duration_variance(index: dict[str, Any]) -> dict[str, dict[str, float]]:
    """Per version: min / max / mean duration across the runs that included it."""
    runs = index["runs"]
    out: dict[str, dict[str, float]] = {}
    versions = sorted({v for run in runs for v in run["versions"]})
    for version in versions:
        samples = [
            float(run["versions"][version]["duration_s"])
            for run in runs
            if version in run["versions"]
        ]
        if samples:
            out[version] = {
                "runs": len(samples),
                "min_s": min(samples),
                "max_s": max(samples),
                "mean_s": round(sum(samples) / len(samples), 2),
            }
    return out


def first_pass_trend(index: dict[str, Any]) -> list[dict[str, Any]]:
    """First-pass rate per run, in order. The headline health number over time."""
    return [
        {"run_id": run["run_id"], "started": run["started"], "rate": run["first_pass_rate"]}
        for run in index["runs"]
        if run["first_pass_rate"] is not None
    ]


def comparison(now: datetime | None = None) -> dict[str, Any]:
    """Everything the cross-run panels need, from the index alone."""
    index = build_index(now)
    return {
        "runs": len(index["runs"]),
        "heatmap": failure_heatmap(index),
        "durations": duration_variance(index),
        "first_pass": first_pass_trend(index),
        "single_run": len(index["runs"]) < 2,
    }
