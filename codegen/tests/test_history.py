"""TRK-022 — cross-run index and comparison.

The criterion that matters: the heatmap must distinguish "0 failures" from "this
version did not run". They are different claims, and conflating them turns the most
valuable panel in the project into a lie.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime
from pathlib import Path

from tests import gen_log
from tracker import history, paths

NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)


def _seed(run_id: str, preset: str) -> None:
    paths.run_dir(run_id).mkdir(parents=True, exist_ok=True)
    paths.events_path(run_id).write_text(gen_log.preset(preset), encoding="utf-8")


def test_index_rebuilds_from_run_directories_alone(isolated_runs_dir: Path) -> None:
    _seed("run-20260803-142012", "clean-run")
    _seed("run-20260804-090000", "retry-run")

    first = history.write_index(NOW)
    (paths.runs_root() / "index.json").unlink()
    second = history.build_index(NOW)

    assert json.dumps(first["runs"]) == json.dumps(second["runs"])
    assert len(second["runs"]) == 2


def test_the_heatmap_separates_zero_failures_from_not_run(isolated_runs_dir: Path) -> None:
    """The distinction the panel exists for."""
    _seed("run-20260803-142012", "clean-run")     # v01.01 + v01.02, no retries
    _seed("run-20260804-090000", "retry-run")     # v01.01 only, with retries

    heat = history.failure_heatmap(history.build_index(NOW))

    assert heat["v01.01"]["run-20260803-142012"] == 0, "ran, no failures"
    retried = heat["v01.01"]["run-20260804-090000"]
    assert retried is not None and retried > 0, "ran, failed"
    assert heat["v01.02"]["run-20260804-090000"] is history.NOT_RUN, (
        "did not run -- must not read as a clean zero"
    )


def test_duration_variance_reports_spread_per_version(isolated_runs_dir: Path) -> None:
    _seed("run-20260803-142012", "clean-run")
    _seed("run-20260804-090000", "clean-run")
    variance = history.duration_variance(history.build_index(NOW))
    assert variance["v01.01"]["runs"] == 2
    row = variance["v01.01"]
    assert row["min_s"] <= row["mean_s"] <= row["max_s"]


def test_first_pass_trend_is_ordered_by_run(isolated_runs_dir: Path) -> None:
    _seed("run-20260803-142012", "clean-run")
    _seed("run-20260804-090000", "retry-run")
    trend = history.first_pass_trend(history.build_index(NOW))
    assert [t["run_id"] for t in trend] == ["run-20260803-142012", "run-20260804-090000"]
    assert trend[0]["rate"] == 1.0 and trend[1]["rate"] < 1.0


def test_a_single_run_is_flagged_rather_than_charted(isolated_runs_dir: Path) -> None:
    """One run is an anecdote; the panels must say so instead of drawing one point."""
    _seed("run-20260803-142012", "clean-run")
    assert history.comparison(NOW)["single_run"] is True
    _seed("run-20260804-090000", "clean-run")
    assert history.comparison(NOW)["single_run"] is False


def test_comparison_is_empty_but_valid_with_no_runs(isolated_runs_dir: Path) -> None:
    result = history.comparison(NOW)
    assert result["runs"] == 0 and result["heatmap"] == {} and result["single_run"] is True
