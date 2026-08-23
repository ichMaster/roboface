"""TRK-019 / TRK-020 / TRK-021 — the dashboard server, its frames, and the palette.

The load-bearing test is the first: the dashboard must serve with the entire generated
application absent, because that is its normal state between runs. A dashboard that
needs the tree it is watching would be deleted by the process it exists to observe.
"""

from __future__ import annotations

import copy
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import pytest

from tests import gen_log
from tracker import paths
from tracker.reduce import reduce

fastapi = pytest.importorskip("fastapi", reason="dashboard deps are optional (requirements.txt)")
from fastapi.testclient import TestClient  # noqa: E402

from dashboard import server  # noqa: E402

STATIC = paths.codegen_root() / "dashboard" / "static"


@pytest.fixture
def seeded(isolated_runs_dir: Path) -> str:
    run_id = gen_log.RUN_ID
    paths.events_path(run_id).parent.mkdir(parents=True, exist_ok=True)
    paths.events_path(run_id).write_text(gen_log.preset("clean-run"), encoding="utf-8")
    paths.current_pointer().write_text(run_id, encoding="utf-8")
    return run_id


# ── independence from the generated application ──────────────────────────────


def test_the_dashboard_imports_nothing_from_the_generated_app() -> None:
    """server/, games/, agent/ may not exist. Asserted over the source, not by trying."""
    offenders: list[str] = []
    for path in (paths.codegen_root()).rglob("*.py"):
        if "__pycache__" in path.parts:
            continue
        text = path.read_text(encoding="utf-8")
        for module in ("server.", "games.", "agent.", "web."):
            if re.search(rf"^\s*(from|import)\s+{re.escape(module)}", text, re.MULTILINE):
                offenders.append(f"{path.name}: {module}")
    assert not offenders, offenders


def test_it_serves_with_the_application_tree_absent(tmp_path: Path) -> None:
    """Serve from a tree that genuinely has no application beside it.

    This must *construct* the absence rather than assert it of the live repo. A
    generation run creates ``server/``, ``games/``, ``agent/`` and ``web/`` -- that is
    what a run is -- so a check on the real directory only passes *between* runs,
    which is precisely when the coupling it guards against cannot bite. Asserting
    the ambient state would therefore go quiet exactly when it mattered.

    So: copy ``codegen/`` somewhere with no application siblings, seed a run inside
    it, and serve from there in a subprocess -- a fresh interpreter, so the import is
    real and ``codegen_root()`` resolves into the sandbox.
    """
    sandbox = tmp_path / "sandbox"
    shutil.copytree(
        paths.codegen_root(),
        sandbox / "codegen",
        ignore=shutil.ignore_patterns("__pycache__", "runs", "var", "tests", ".*_cache"),
    )

    runs = sandbox / "codegen" / "runs"
    (runs / gen_log.RUN_ID).mkdir(parents=True)
    (runs / gen_log.RUN_ID / "events.jsonl").write_text(
        gen_log.preset("clean-run"), encoding="utf-8"
    )
    (runs / "current").write_text(gen_log.RUN_ID, encoding="utf-8")

    present = [d for d in ("server", "games", "agent", "web") if (sandbox / d).exists()]
    assert not present, f"the sandbox must have no application tree, found {present}"

    env = {k: v for k, v in os.environ.items() if k != paths.RUNS_DIR_ENV}
    result = subprocess.run(
        [sys.executable, "-c", _SERVE_PROBE],
        cwd=sandbox,
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )
    assert result.returncode == 0, f"stdout={result.stdout}\nstderr={result.stderr}"
    assert result.stdout.strip().endswith("SERVED"), result.stdout


#: Run inside the sandbox: import the dashboard with no application tree in sight and
#: prove it both starts and answers. Kept as a constant so the test above stays readable.
_SERVE_PROBE = """
import sys
from pathlib import Path

sys.path.insert(0, "codegen")

from fastapi.testclient import TestClient
from tracker import paths
from dashboard import server

root = paths.codegen_root()
assert root == Path.cwd() / "codegen", root
for directory in ("server", "games", "agent", "web"):
    assert not (root.parent / directory).exists(), directory

with TestClient(server.app) as client:
    assert client.get("/").status_code == 200
    assert client.get("/api/state").status_code == 200
print("SERVED")
"""


def test_it_uses_its_own_port_never_the_apps() -> None:
    assert server.PORT == 8420, "8000 belongs to the generated app; both may run at once"


# ── state and frames ─────────────────────────────────────────────────────────


def test_api_state_returns_the_reduced_run(seeded: str) -> None:
    with TestClient(server.app) as client:
        payload = client.get("/api/state").json()
    assert payload["run_id"] == seeded
    assert payload["metrics"]["issues_done"] == 7
    assert payload["status"] == "done"


def test_api_state_is_sane_when_no_run_exists(isolated_runs_dir: Path) -> None:
    with TestClient(server.app) as client:
        payload = client.get("/api/state").json()
    assert payload["status"] == "no-runs"
    assert payload["tree"] == []


def test_websocket_sends_a_snapshot_first(seeded: str) -> None:
    with TestClient(server.app) as client, client.websocket_connect("/ws") as ws:
        frame = ws.receive_json()
    assert frame["kind"] == "snapshot"
    assert frame["state"]["run_id"] == seeded


def test_frame_shape_matches_the_specification(seeded: str) -> None:
    """dashboard-specification §6.1: {kind, state, event?} and nothing else."""
    with TestClient(server.app) as client, client.websocket_connect("/ws") as ws:
        frame = ws.receive_json()
    assert set(frame) <= {"kind", "state", "event"}
    assert frame["kind"] in {"snapshot", "delta"}


def test_a_client_connecting_late_converges_to_the_same_state(seeded: str) -> None:
    """Reconnect re-requests a snapshot, so no per-client cursor is needed (§6.2)."""
    with TestClient(server.app) as client:
        with client.websocket_connect("/ws") as first:
            early = first.receive_json()["state"]
        with client.websocket_connect("/ws") as second:
            late = second.receive_json()["state"]
    assert early["metrics"] == late["metrics"]
    assert early["tree"] == late["tree"]


def test_api_runs_lists_runs(seeded: str) -> None:
    with TestClient(server.app) as client:
        payload = client.get("/api/runs").json()
    assert seeded in payload["runs"]
    assert payload["active"] == seeded


# ── the static bundle ────────────────────────────────────────────────────────


def test_the_ui_is_split_into_three_files_with_no_build_step() -> None:
    for name in ("index.html", "app.js", "styles.css"):
        assert (STATIC / name).is_file(), name


def test_no_external_requests_anywhere_in_the_bundle() -> None:
    """A strict reading of containment: the page must work offline."""
    for name in ("index.html", "app.js", "styles.css"):
        text = (STATIC / name).read_text(encoding="utf-8")
        assert "http://" not in text and "https://" not in text, name
        assert "cdn" not in text.lower(), name


def test_the_page_references_only_local_assets() -> None:
    html = (STATIC / "index.html").read_text(encoding="utf-8")
    for match in re.findall(r'(?:src|href)="([^"]+)"', html):
        assert match.startswith("/static/"), match


def test_app_js_is_valid_javascript() -> None:
    node = shutil.which("node")
    if not node:
        pytest.skip("node unavailable")
    result = subprocess.run(
        [node, "--check", str(STATIC / "app.js")], capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr


def test_every_chart_has_a_table_view_toggle() -> None:
    """The WCAG-clean twin is not optional on any panel (spec §4.9)."""
    html = (STATIC / "index.html").read_text(encoding="utf-8")
    charts = set(re.findall(r'id="c-(\w+)"', html))
    tables = set(re.findall(r'id="tv-(\w+)"', html))
    assert charts and charts == tables, (charts, tables)


def test_reduced_motion_and_focus_rings_are_honoured() -> None:
    css = (STATIC / "styles.css").read_text(encoding="utf-8")
    assert "prefers-reduced-motion" in css
    assert "focus-visible" in css
    assert "outline:none" not in css.replace(" ", "")


# ── TRK-021: the palette must stay validated ─────────────────────────────────

VALIDATOR = Path(
    "/private/tmp/claude-502/bundled-skills/2.1.220/"
    "a580c4f332c8630ce8703970b2ee2a79/dataviz/scripts/validate_palette.js"
)


def _series(mode: str) -> list[str]:
    css = (STATIC / "styles.css").read_text(encoding="utf-8")
    block = css.split(':root[data-theme="dark"]')[1] if mode == "dark" else css.split("@media")[0]
    found = dict(re.findall(r"--series-(\d):\s*(#[0-9a-fA-F]{6})", block))
    return [found[str(i)] for i in sorted(found) if i in "12345"]


@pytest.mark.parametrize("mode", ["light", "dark"])
def test_the_shipped_palette_still_passes_the_validator(mode: str) -> None:
    """A colour tweak must not silently break CVD safety."""
    series = _series(mode)
    assert len(series) == 5, f"expected 5 series slots in {mode}, got {series}"
    node = shutil.which("node")
    if not node or not VALIDATOR.is_file():
        pytest.skip("palette validator unavailable")
    result = subprocess.run(
        [node, str(VALIDATOR), ",".join(series), "--mode", mode],
        capture_output=True, text=True,
    )
    assert "FAIL" not in result.stdout, result.stdout


def test_the_css_palette_matches_the_specification_table() -> None:
    """The spec table and the shipped CSS are one palette; drift fails here."""
    spec = (paths.codegen_root() / "dashboard-specification.md").read_text(encoding="utf-8")
    row = next(line for line in spec.splitlines() if line.startswith("| `--series-1…5`"))
    documented = re.findall(r"#[0-9a-fA-F]{6}", row)
    assert documented[:5] == _series("light")
    assert documented[5:10] == _series("dark")


def test_replay_drives_a_live_update(isolated_runs_dir: Path) -> None:
    """TRK-024's second half: the only way to exercise motion before instrumentation."""
    from tests.replay import replay

    lines = gen_log.preset("clean-run").splitlines()[:12]
    count = replay(lines, gen_log.RUN_ID, speed=1000.0)
    assert count == 12
    with TestClient(server.app) as client:
        assert client.get("/api/state").json()["counts"]["events"] == 12


def test_dashboard_writes_only_under_codegen(seeded: str) -> None:
    """Containment: reads runs/, writes var/, touches nothing else."""
    source = (paths.codegen_root() / "dashboard" / "server.py").read_text(encoding="utf-8")
    assert "var_root()" in source
    assert "/tmp" not in source
    assert json.dumps(source).count("os.path.expanduser") == 0


# ── the snapshot is a cache, and a cache that never invalidates is a frozen page ──


def test_a_hand_rebuilt_snapshot_does_not_freeze_the_dashboard(seeded: str) -> None:
    """The failure this guards: a live-looking page whose numbers never move.

    Nothing in the pipeline writes `state.json`, so the dashboard normally reduces the
    log every time. But `python3 -m tracker.state` creates one, and a reader that trusted
    it blindly served that instant forever -- while the WebSocket kept pushing a frame on
    every append, so the page advertised itself as live the whole time.
    """
    from tracker import state as state_mod

    lines = gen_log.preset("clean-run").splitlines()
    events = paths.events_path(seeded)
    events.write_text("\n".join(lines[:10]) + "\n", encoding="utf-8")

    state_mod.rebuild(seeded)  # the hand rebuild that used to poison every later read
    with TestClient(server.app) as client:
        assert client.get("/api/state").json()["counts"]["events"] == 10

    events.write_text("\n".join(lines[:20]) + "\n", encoding="utf-8")
    with TestClient(server.app) as client:
        assert client.get("/api/state").json()["counts"]["events"] == 20, (
            "the log grew and the dashboard kept serving the snapshot"
        )


def test_a_snapshot_newer_than_the_log_is_still_used(seeded: str) -> None:
    """The fix must not throw the cache away entirely -- only invalidate it."""
    from tracker import state as state_mod

    state_mod.rebuild(seeded)
    assert not state_mod.is_stale(seeded)
    assert state_mod.read(seeded) is not None


def test_a_missing_snapshot_reads_as_stale(seeded: str) -> None:
    """Absent and stale deserve one answer: reduce the log, which is the source of truth."""
    from tracker import state as state_mod

    assert state_mod.is_stale(seeded)
    assert state_mod.read(seeded) is None


# ── panel geometry: the charts must survive a run wider than the mock ────────
#
# Every fault these guard against reached a real screen. The prototype's mock had four
# versions and the fixtures had two, so panels that divided a FIXED height by their row
# count, and axes with maxima copied from that mock, were correct on every test that
# existed. A ten-version run gave the bars a negative height (they vanished, and the
# labels landed on top of each other) and sent the suite series far above its own card,
# where -- SVG does not clip by default -- it painted over the panel above.

RENDERER = Path(__file__).resolve().parent / "render_panels.js"


def _render(state: dict[str, Any]) -> dict[str, Any]:
    """Run the real app.js against a state object and report where the ink landed."""
    node = shutil.which("node")
    if not node:
        pytest.skip("node unavailable")
    result = subprocess.run(
        [node, str(RENDERER)], input=json.dumps(state),
        capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stderr
    report: dict[str, Any] = json.loads(result.stdout)
    return report


def _panels(report: dict[str, Any]) -> dict[str, Any]:
    """Just the charts. Underscore keys are whole-page answers, not panels."""
    return {k: v for k, v in report.items() if not k.startswith("_")}


@pytest.fixture
def wide_state() -> dict[str, Any]:
    """Ten versions across three phases — the real validation workload's shape."""
    lines = gen_log.preset("full-roadmap").splitlines()
    state = reduce(lines, datetime(2026, 8, 3, 18, 0, tzinfo=UTC))
    state.run_id = "run-20260803-120000"
    return state.as_dict()


def test_no_panel_draws_outside_its_own_viewbox(wide_state: dict[str, Any]) -> None:
    """The overflow that put one chart's series on top of another chart."""
    for panel, r in _panels(_render(wide_state)).items():
        assert r["rendered"], f"{panel} did not render: {r.get('html')}"
        assert r["min_y"] >= -0.5, f"{panel} draws above its box at y={r['min_y']}"
        assert r["max_y"] <= r["height"] + 0.5, (
            f"{panel} draws below its box: {r['max_y']} > {r['height']}"
        )


def test_velocity_survives_a_version_with_zero_closed_issues(
    wide_state: dict[str, Any],
) -> None:
    """A version whose steps have started but has closed no issues yet -- e.g. still
    in generate-issues -- gave this chart a NaN mean (0/0). Math.max(...) over the
    whole row set is NaN for one NaN input, and niceMax's `v || 0` silently floors
    that to its 60s minimum, so every *real* bar then computes against a collapsed
    axis and paints far outside the card (SVGs don't clip by default). Reproduces
    the exact live shape: a running version with a generate-issues step but no
    execute-issues step at all yet, so it contributes no issues."""
    state = copy.deepcopy(wide_state)
    state["plan"] = [*state["plan"], "v04.01"]
    state["tree"][0]["children"].append({
        "id": "v04.01", "kind": "version", "status": "running",
        "start": "2026-08-03T18:00:00.000Z", "end": None, "elapsed_s": 90.0,
        "children": [
            {"id": "generate-issues", "kind": "step", "status": "ok",
             "start": "2026-08-03T18:00:00.000Z", "end": "2026-08-03T18:01:30.000Z",
             "elapsed_s": 90.0, "children": []},
        ],
    })
    for panel, r in _panels(_render(state)).items():
        assert r["rendered"], f"{panel} did not render: {r.get('html')}"
        assert r["min_y"] >= -0.5, f"{panel} draws above its box at y={r['min_y']}"
        assert r["max_y"] <= r["height"] + 0.5, (
            f"{panel} draws below its box: {r['max_y']} > {r['height']}"
        )


def test_every_bar_has_a_positive_height(wide_state: dict[str, Any]) -> None:
    """Ten rows in a height laid out for four made bars negative — invisible, silently."""
    for panel, r in _panels(_render(wide_state)).items():
        bad = [h for h in r["bar_heights"] if h <= 0]
        assert not bad, f"{panel} drew {len(bad)} bar(s) with height <= 0"


def test_row_labels_do_not_collide(wide_state: dict[str, Any]) -> None:
    """Horizontal panels: consecutive row labels need vertical room for their text."""
    for panel, r in _panels(_render(wide_state)).items():
        rows = r["label_rows"]
        gaps = [round(b - a, 2) for a, b in zip(rows, rows[1:], strict=False)]
        assert all(g >= 12 for g in gaps), f"{panel} row labels overlap: gaps {gaps}"


def test_x_axis_category_labels_do_not_collide(wide_state: dict[str, Any]) -> None:
    """Ten version names in a narrow card must rotate rather than overrun each other."""
    for panel, r in _panels(_render(wide_state)).items():
        labels = r["x_labels"]
        if len(labels) < 2:
            continue
        pitch = min(b["x"] - a["x"] for a, b in zip(labels, labels[1:], strict=False))
        # Rotated labels cannot collide with their neighbour; only upright ones can.
        widest = max((len(lbl["text"]) * 6.2 for lbl in labels if not lbl["rotated"]),
                     default=0.0)
        assert widest <= pitch, (
            f"{panel}: horizontal labels need {widest:.0f}px but the slot is {pitch:.0f}px"
        )


def test_a_clock_never_reads_sixty_seconds(wide_state: dict[str, Any]) -> None:
    """`26:60` — minutes floored and seconds rounded independently split 1619.7s wrong.

    A clock that can print :60 quietly discredits every other figure on the page.
    """
    bad = _render(wide_state)["_clock"]
    assert bad == [], f"mmss() produced invalid times, e.g. {bad[:5]}"


def test_the_suite_trajectory_is_per_version_not_one_repeated_number(
    wide_state: dict[str, Any],
) -> None:
    """The panel drew a dead-flat line that looked like a measurement.

    `tests_passing` is a single running figure. The adapter handed the same final value
    to all ten versions, so "Tests passing, by version" was a straight line by
    construction — it could not have shown anything else, whatever the run did.
    """
    sizes = [
        v["data"]["tests_passing"]
        for phase in wide_state["tree"]
        for v in phase.get("children", [])
        if "tests_passing" in v.get("data", {})
    ]
    assert len(sizes) == 10, f"expected a count per version, got {sizes}"
    assert len(set(sizes)) > 1, "every version reported the same suite size"
    assert sizes == sorted(sizes), f"the suite shrank across versions: {sizes}"

    # ...and that the panel actually plots them. The reducer recording per-version
    # counts is only half the fix: the adapter handed the panel one global figure, and
    # a flat line drawn from real state is indistinguishable from a real flat result.
    plotted = _render(wide_state)["c-suite"]["point_ys"]
    assert len(plotted) == 10, f"expected 10 plotted points, got {len(plotted)}"
    assert len(set(plotted)) > 1, "the suite series is flat — one value repeated"


# ── the clock must keep moving when the log is quiet ─────────────────────────


def test_a_frame_arrives_even_when_the_log_does_not_grow(
    seeded: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Frames used to be sent only on growth, and real runs go quiet for a long time.

    In the v01–v03 run, 23 gaps between events ran over a minute and the longest was 26.
    With no frame the header's elapsed clock stops — it is computed at reduce time — so
    the page sat motionless for 26 minutes with the socket up and the pipeline working.

    Read on a background thread with a deadline: without a heartbeat `receive_json`
    blocks forever, and a test that hangs the suite is worse than no test at all.
    """
    monkeypatch.setattr(server, "HEARTBEAT_SECONDS", 0.05)
    monkeypatch.setattr(server, "POLL_SECONDS", 0.01)
    before = paths.events_path(seeded).stat().st_size
    box: queue.Queue[Any] = queue.Queue()

    def read_three() -> None:
        try:
            with TestClient(server.app) as client, client.websocket_connect("/ws") as ws:
                # Three, so this cannot pass on a frame sent for some other reason.
                box.put([ws.receive_json() for _ in range(3)])
        except BaseException as exc:  # noqa: BLE001 - reported through the queue
            box.put(exc)

    threading.Thread(target=read_three, daemon=True).start()
    try:
        frames = box.get(timeout=10)
    except queue.Empty:
        pytest.fail("no frame arrived while the log was quiet — the heartbeat is gone")
    assert not isinstance(frames, BaseException), frames

    assert [f["kind"] for f in frames] == ["snapshot", "delta", "delta"]
    assert frames[-1]["state"]["run_id"] == seeded
    assert paths.events_path(seeded).stat().st_size == before, "the log must not have grown"


def test_the_heartbeat_does_not_restart_a_finished_run_s_clock(seeded: str) -> None:
    """A repeated frame for a done run must be identical, not a clock creeping upward.

    elapsed is measured to `ended` when there is one, so this holds without a special
    case in the loop — but only as long as that stays true.
    """
    first = server.current_state(seeded)
    second = server.current_state(seeded)
    assert first["status"] == "done"
    assert first["elapsed_s"] == second["elapsed_s"]
