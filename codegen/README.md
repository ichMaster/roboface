# codegen — tracking the code generation, and the dashboard that shows it

This folder is the **subject-independent half** of the repo. Everything the SDLC skills in
`.claude/skills/` build from `specification/` — `server/`, `firmware/`, `assets/`, `tests/` — is
generated output. This folder watches that happen and records it.

Nothing here imports from the generated code, and nothing here is deleted when a run is reset. That
is deliberate: the dashboard must start and serve with the whole product tree absent, which is its
state before the first run.

> **Imported into RoboFace** from the `agent-arena-sandbox` project, with the issue prefix retargeted
> to `RF-###`. The design documents below still narrate the run they were written against (an
> `v01.01…v05.03` build of a different application) — that is kept as the evidence the design was
> derived from, not as a description of this repo. What applies here is the machinery: the event
> contract, the emitter, the hooks, the reducer and the dashboard. RoboFace numbers its versions
> `vA.B` and releases them `vA.B.C` (see [specification/ROADMAP.md](../specification/ROADMAP.md)).

`runs/` is **gitignored** — a log is evidence produced on one machine, and a fresh clone has
none. There is nothing to look at until you run a pipeline, or replay a log you kept.

```
codegen/
├── tracker/      the event log: emit → reduce → state       (stdlib only)
├── hooks/        Claude Code hooks — the independent floor  (stdlib only)
├── dashboard/    FastAPI server + a no-build page on :8420
├── runs/         one directory per run — the logs ARE the product (gitignored)
└── tests/        290 tests
```

---

## The dashboard

### Install

The tracker and the hooks are **stdlib-only on purpose** — they run on the pipeline's critical
path, and an emitter that cannot import is an emitter that can break a build. Only the
dashboard and the test suite have dependencies:

```bash
python3 -m venv .venv                       # from the repo root
.venv/bin/pip install -r codegen/requirements.txt
```

Install into a venv of your own — never into the generated project's, and never via the
repo-root `pyproject.toml`, which the pipeline regenerates and overwrites.

### Run

```bash
cd codegen
../.venv/bin/python -m uvicorn dashboard.server:app --port 8420
```

Then open **<http://127.0.0.1:8420/>**.

Port **8420**, never 8000 — the RoboFace server owns 8000 and both may run at once.

You can start it **at any time**: before a run, during one, or long after. It picks up the
active run from `runs/current`, and falls back to the most recent run when nothing is
running, so there is no "too late". Leaving it running across a whole `/ship-phase` is the
intended use.

### What it is showing you

Every figure is **reduced from `runs/<run-id>/events.jsonl`** — the run's own append-only
log — by `tracker/reduce.py`. Nothing is sampled from the working tree. That is the point:
the dashboard and the repo are two independent records, so when they disagree, the
disagreement is itself the finding. Do not "fix" the log to match the tree.

The page opens a WebSocket to `/ws`, gets a full snapshot on connect, then a fresh one
whenever the log grows (polled every 0.4 s) **and at least every 5 seconds regardless**. The
heartbeat is what keeps the elapsed clock moving: a real run is quiet for long stretches — in
the v01–v03 run 23 gaps between events ran over a minute and the longest was 26 — and elapsed
is computed at reduce time, so without a frame the page freezes while the pipeline works.

So: on a busy stretch the numbers move within half a second of the event; when nothing is
happening, the clock still ticks every 5 seconds. A finished run needs no special case — its
elapsed is measured to `ended`, so it stops on its own.

On disconnect the page **holds the last render** and reconnects with backoff — it never
blanks. The indicator next to the title reads `live` or `reconnecting`.

### The panels, top to bottom

| Panel | What it answers | How to read it |
|---|---|---|
| **Run header** | How long, where are we now, when will it end | `Now` names the *deepest running node* (`v01.03 · RF-016 · validating`). **ETA is a range, never a point**, and is blank until at least one version has finished — an ETA computed from nothing is worse than no ETA. |
| **KPI row** | The eight numbers worth a glance | Issues done · versions released / planned · mean time per issue (with the delta against the *previous version*, direction stated in words) · tests passing · tests failing now · review findings open/raised · GitHub issues created/closed/open · commits. |
| **Live run tree** | Exactly where the pipeline is | Five nested levels — run → phase → version → step → issue — because five levels of state is more than colour can carry. The running branch is expanded; released versions carry their tag. |
| **Burn-down** | Work remaining vs. ideal | Scope is **discovered, not known**: versions that haven't been decomposed yet contribute an uncertainty band. **The band should be widest at the start and narrow as `generate-issues` runs.** If it doesn't, `version.decomposed` isn't being emitted. |
| **Velocity** | Mean time per issue, per version | Dots show the per-issue spread. A mean alone hides its retries — look at the dots before believing the line. |
| **Where the time went** | Which of the five steps costs what | Steps are strictly gated, so this composition *is* the chronology; no Gantt needed. |
| **Failure surface** | Which issues fought back | Emphasis, not eight hues: issues needing more than one attempt are highlighted, first-pass issues are de-emphasised. |
| **Tests passing, by version** | Suite trajectory | The suite size **as each version left it**, stamped at `version.end`. Suite *duration* is deliberately **not** on this chart — tests against seconds on a second axis would invent a correlation. |
| **Code-review findings** | How many, and what closed them | Fixed now / hardened later / still deferred / held. **Review density** (findings ÷ issues shipped) is the comparable number; raw counts just track version size. |

Every chart has a **Table** button — the same data as rows, which is also the accessible path.

If a panel throws, it says so in red at the foot of the page instead of blanking it. An
observability tool that hides its own faults is the worst kind.

### Reading it without the browser

The same objects the page renders, as JSON:

```bash
curl -s localhost:8420/api/state              # the active (or most recent) run
curl -s localhost:8420/api/state?run_id=run-20260803-160515
curl -s localhost:8420/api/runs               # every run + which is active
curl -s localhost:8420/api/history            # cross-run comparison
```

`/api/history` reports `single_run: true` when only one run exists, so the UI says so rather
than drawing a one-point trend line.

A quick health read:

```bash
curl -s localhost:8420/api/state | python3 -c "import json,sys;d=json.load(sys.stdin);\
print(d['run_id'],d['status']);print(d['metrics']);print(d['counts'])"
```

`counts` is the one to watch: `torn` and `malformed` must both be `0`, and `quarantine` must
be empty. Anything else means events were written that the schema does not accept.

Charts size themselves to the data: axis maxima come from the values, and the horizontal
panels grow with their row count. Nothing is laid out for a fixed number of versions — the
`full-roadmap` fixture (ten versions, ~450 tests) exists so the tests exercise a real run's
shape, and `tests/render_panels.js` renders every panel headlessly to assert no series
escapes its card, no bar has zero height, and no label lands on its neighbour.

### Known gaps

- **The filter row (Run / Phase / Status) is not wired.** The selects render but do nothing.
  Use `?run_id=` on `/api/state` to inspect another run in the meantime.
- **One run at a time.** The page follows the active run; comparing two runs side by side is
  `/api/history` only.

---

## Starting a run

```
/ship-phase v03
```

Ten versions, v01.01 → v03.03. Dependency fill adds v01 and v02 on its own, so naming the
last phase is the whole command.

**Check first — one of these silently does nothing rather than failing:**

```bash
git status --short        # must be clean
git tag                   # see below
gh auth status            # upload-issues needs it
python3 -c "import json;print(json.load(open('.claude/settings.json'))['hooks'].keys())"
```

`git tag` is the trap. **Both orchestrators skip any version whose release tag already
exists**, so a repo carrying tags from a previous run will skip those versions and report
success having built nothing. After a `/reset-generated` the tags are gone; if you are
re-running without a reset, delete them by hand first.

The hooks load at **session start**. Editing `.claude/settings.json` mid-session leaves the
independent floor absent, and `reconcile` then has nothing to measure the skills against —
restart the session after changing it.

Two behaviours worth checking early, because they fail quietly: the burn-down's uncertainty
band should be **widest at the start** and narrow as each version decomposes, and the `Now`
line should name the **deepest running node**, not a finished one.

---

## The other tools

```bash
python3 -m tracker.reconcile [run-id]
```

Compares what the **skills** recorded against what the **hooks** independently saw, and
writes a report to `var/reconcile-<run-id>.md`. Two rates:

- **emit** — of the issues where a hook saw `pytest` run, how many emitted `issue.validate.end`.
  Scoped per open issue, so baselines and ad-hoc shells don't drag it down.
- **commits** — distinct shas claimed in the log vs. distinct shas in git.

A *falling* emit rate is the finding: it means the skill files have grown too long for their
own instructions to survive. Fix the emit, never the log.

```bash
python3 -m tracker.state [run-id]     # rebuild state.json from the log
python3 -m tracker.emit <type> --emitter skill:x --scope k=v,... [--status ok] [--data '{}']
```

`emit` never raises and never blocks — a tracking failure cannot fail a pipeline step. The
skills call it; you rarely need to.

`state.json` is a **disposable cache**, not a record: nothing in the pipeline writes one, so
the dashboard normally reduces the log on every request. `tracker.state` creates one by hand.
It invalidates itself as soon as the log grows, so a stale snapshot can't freeze the page —
and deleting it loses nothing.

`/reset-generated` clears what a run created, reading the run's own log: the log names the
commits (and the tags, which name the release commits), and `git show --diff-filter=A` names
the files those commits *added*. The skill carries the commands — there is no script.

It never touches `codegen/` (the logs are the product), `.claude/` (the skills are source),
`specification/` (the source of intent), `spec/` (the original concept), `.env*` / `.envrc` /
`.gitignore` (a deleted key may not be recoverable), or GitHub issues (they carry the `RF-###`
counter). Anything the log does not account for is **left alone and reported**.



---

## Working on the tracker itself

```bash
cd codegen
../.venv/bin/pytest            # 314 tests
../.venv/bin/mypy .            # strict
../.venv/bin/ruff check .
```

Run pytest **from inside `codegen/`** (or with `-c codegen/pyproject.toml`). From the repo
root, plain `pytest` collects the server's suite too and merges the two counts.

mypy reads config from the current directory only — it does not walk up. From the repo root
it must be `mypy --config-file codegen/pyproject.toml codegen/`, or it silently runs
non-strict.

`CODEGEN_RUNS_DIR` redirects both `runs/` and `var/`, which is how the tests never touch the
real log.

### The two rules that keep this honest

1. **`tracker/` and `hooks/` import nothing outside the stdlib.** They run on the pipeline's
   critical path. A test asserts it.
2. **The hooks are an independent floor.** They are registered in `.claude/settings.json` and
   load at session start, so a settings change needs a session restart to take effect. They
   observe tool use without knowing what the skills claim — which is the only reason
   `reconcile` can measure anything at all.

---

## The documents

| | |
|---|---|
| [ship-phase-tracking-vision.md](ship-phase-tracking-vision.md) | why this exists, and what each panel is for |
| [architecture.md](architecture.md) | the event contract, log format, guarantees, test strategy |
| [dashboard-specification.md](dashboard-specification.md) | how the UI is built, and the design rules behind it |
| [implementation-plan.md](implementation-plan.md) | the 24 build tasks, and the validation workload |
| [device-frontends-vision.md](device-frontends-vision.md) | design vision — a Core2 desk display and a StickC Plus2 pager as ambient frontends over BLE (not built) |
| [m5-implementation-plan.md](m5-implementation-plan.md) | the 20 `M5-###` tasks for the bridge and the firmware — 12 need no hardware |
