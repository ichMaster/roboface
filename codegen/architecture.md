# Tracking system — architecture

**Status:** design. Nothing here is implemented yet.
**Companion:** [ship-phase-tracking-vision.md](ship-phase-tracking-vision.md) is the *why* — the problem,
the event model, the dashboard's panels. This document is the *how*: contracts, formats, guarantees,
and the test strategy. Where the two disagree, this file wins on mechanics and the vision doc wins on
intent.

> **Not to be confused with [`specification/ARCHITECTURE.md`](../specification/ARCHITECTURE.md)**, which is the
> architecture of the *generated application*. This system observes that one and shares no code with it.

---

## 1. Components

One direction of dependency throughout. Nothing downstream is required for the pipeline to run.

```
 skills ──emit──┐                                                     ┌──► browser
                ├──► events.jsonl ──► reducer ──► state.json ──► dashboard ──WS──┐
 hooks  ──emit──┘     (append-only)    (pure)      (snapshot)         │          │
                            ▲                                         └──────────┴──► bridge
 git ───────────────────────┘  (reconciliation only, after the fact)                      │
                                                                              BLE, device-polled
                                                                                          ▼
                                                                          Core2 · StickC Plus2
```

| Component | Path | Responsibility |
|---|---|---|
| **Emitter** | `codegen/tracker/emit.py` | The *only* writer. Appends one validated event. Never raises. |
| **Schema** | `codegen/tracker/schema.json` | The event contract, versioned. |
| **Reducer** | `codegen/tracker/reduce.py` | Pure `events → state`. No I/O beyond reading the log. |
| **Hooks** | `codegen/hooks/*.py` | Harness-invoked; translate tool calls into events. |
| **Dashboard** | `codegen/dashboard/server.py` | Tails the log, serves the UI, pushes over WS. |
| **Bridge** | `codegen/bridge/` | BLE central. Subscribes to the dashboard's WS, answers device polls with per-screen JSON. §1.2 |
| **Firmware** | `codegen/device/` | Renders those frames on an M5Stack panel. Not Python; see §10.8. |
| **Reset** | `codegen/reset.py` | Deletes what a run created, read from its own log (skill: `reset-generated`). |
| **Tests** | `codegen/tests/` | See §10. **Not** the app's `tests/`, which is deleted every run. |

**Dependency rule.** `tracker/` imports nothing. `hooks/` imports `tracker/`. `dashboard/` imports
`tracker/`. `bridge/` imports `tracker/` and talks to `dashboard/` **over HTTP**, never by import.
Nothing imports `server/`, `games/`, or `agent/` — those directories may not exist. Nothing imports
`bridge/`: it is a leaf, and the dashboard must keep starting on a machine with no Bluetooth and no
`bleak` installed.

### 1.2 The bridge is a client, not a stage

The bridge consumes `ws://127.0.0.1:8420/ws` — **the same frames the browser gets** — and projects
them down to per-screen JSON small enough for one BLE write. It is placed there rather than inside
`dashboard/server.py` for four reasons, in order of weight:

- **The frontends cannot disagree.** Browser and panels render one state object, projected
  differently. A device computing its own figures would eventually contradict the screen beside it.
- **`server.py` stays a pure reader.** `bleak` never enters that process.
- **A dead bridge is invisible** — principle 2 (*emission must never gate the pipeline*), one hop
  further out.
- **It is testable without a radio** (§10.8).

The full contract — frame shapes, the poll protocol, per-screen sizes and cadences, the notification
channel — lives in **[device-frontends-vision.md](device-frontends-vision.md)**, which owns it. This
document does not mirror it; §11.1 covers only where the two version numbers meet.

### 1.1 Who builds this — and why not the pipeline

**The tracker is written by ordinary development work, never by `/ship-phase` or `/ship-solution`.**
Those orchestrators build the *application*; this system observes them. Five reasons the boundary is
strict rather than stylistic:

| | |
|---|---|
| **Circularity** | TRK-010–015 modify the very skills that would be doing the building. A broken emit instruction would then break the build of its own fix. |
| **Wrong inputs** | `ship-phase` decomposes `spec/roadmap.md` into `RF-###` issues under `spec/implementation/`. The tracker has no roadmap versions — it has `TRK-###` tasks in `implementation-plan.md`. |
| **Wrong outputs** | `ship-phase` releases per version: `vXX.YY.00` tags, `VERSION` and `RELEASE.txt` bumps. The tracker is not versioned that way. |
| **Contaminated measurement** | A tracker built by an instrumented run would have its own construction in the log — self-reference inside every metric. |
| **Different lifecycle** | The application is deleted and regenerated every run; the tracker must survive exactly that (vision §3 principle 6). |

Two codebases, two plans, two test suites, two lifecycles — and one pipeline, which touches only one
of them.

**The device work is a third, on the same reasoning.** `bridge/` and `device/` are built by ordinary
development under their own namespace, **`M5-###`**, planned in
[m5-implementation-plan.md](m5-implementation-plan.md). Every argument in the table above applies
unchanged, and one is stronger: the lifecycle differs *again*. The tracker is Python that runs in CI;
the device adds C++ firmware, a PlatformIO toolchain, and stages that cannot run without a board
plugged in. Folding those into `implementation-plan.md` — a plan whose status line reads *implemented,
all 24 tasks done* — would also destroy the record of a finished body of work.

Three namespaces, three lifecycles, and no overlap:

| | `RF-###` | `TRK-###` | `M5-###` |
|---|---|---|---|
| Builds | the generated application | the tracker | the bridge and the firmware |
| Plan | `spec/implementation/` | `implementation-plan.md` | `m5-implementation-plan.md` |
| Built by | `/ship-phase` | ordinary development | ordinary development |
| On GitHub | yes | no | no |
| Lifecycle | deleted every run | permanent | permanent |
| Needs hardware | no | no | **yes, from M5-013** |

**No skill builds the tracker — not even a codegen-specific fork.** A fork was considered and rejected:
the decomposition those skills exist to perform is already done (it is `implementation-plan.md`), so a
fork would only re-implement `execute-issues-file` against different paths, and become another artefact
to keep in sync. The tracker is written as ordinary development, following the working discipline
stated in the plan.

**The `TRK-###` namespace stays out of git and GitHub.** No GitHub issues are created for tracker
tasks — `upload-issues` is for `RF-###` only — and commit subjects use conventional prefixes
(`feat(tracker):`, `test(tracker):`) rather than a task id. `git log` therefore never mixes the two id
systems, and the plan file's checkboxes are the record of progress.

**The two test suites are not the same thing and never share a directory:**

| | `tests/` (repo root) | `codegen/tests/` |
|---|---|---|
| Tests | the generated application | the tracker |
| Written by | `execute-issues`, as part of each `RF-###` | ordinary development, as part of each `TRK-###` |
| Lifecycle | deleted and regenerated with the app | permanent |
| Run by | the pipeline's own validation gate | `pytest codegen/tests` |

A tracker test must never depend on the application existing, and an application test must never know
the tracker exists.

**Bridge tests join `codegen/tests/`**, not a third directory: the bridge is Python, it runs under the
same `pytest`, and the autouse fixture that isolates `CODEGEN_RUNS_DIR` protects it for free. Firmware
tests are a separate runner by necessity (§10.8).

---

## 2. Event contract

### 2.1 Envelope

Every line in `events.jsonl` is one JSON object. Field order is not significant; **key set is**.

| Field | Type | Req | Notes |
|---|---|:--:|---|
| `v` | int | ✔ | Schema version. Currently `1`. See §11. |
| `ts` | string | ✔ | ISO-8601 UTC, millisecond precision, `Z`-suffixed. |
| `run_id` | string | ✔ | `run-YYYYMMDD-HHMMSS`, assigned at `run.start`. |
| `type` | string | ✔ | Dotted event type from §3. |
| `scope` | object | ✔ | `{phase?, version?, step?, issue?}` — the path through the tree. |
| `status` | string | — | `ok` \| `fail` \| `skip` \| `held` \| `running`. Absent on `*.start`. |
| `data` | object | — | Type-specific payload (§3). Absent means "no payload". |
| `emitter` | string | ✔ | `skill:<name>` or `hook:<name>`. Needed for the §10.4 reconciliation. |

```json
{"v":1,"ts":"2026-08-03T14:22:31.482Z","run_id":"run-20260803-142012",
 "type":"issue.validate.end","emitter":"skill:execute-issues",
 "scope":{"phase":"v01","version":"v01.01","step":"execute-issues","issue":"RF-003"},
 "status":"fail","data":{"attempt":1,"pytest":{"passed":41,"failed":1,"duration_s":6.1},"mypy":{"errors":0}}}
```

### 2.2 Ordering — `seq` is assigned on read, not on write

> **A `seq` written by the emitter cannot work**, which is why the envelope in §2.1 has no such field.
> Emitters are independent processes — a skill's Bash call, a hook, the orchestrator — with no shared
> counter and no lock, so no writer can know its own sequence number. (An earlier draft of the vision
> doc carried `seq` in the written envelope; it was corrected to match this.)

The resolution: **file line order is the sequence.** The reducer assigns `seq` as the 0-based line
ordinal while reading. Consumers may rely on `seq`; emitters must never write it. Ties in `ts` are
resolved by line order, which is exactly what `seq` was meant to provide.

### 2.3 Scope

`scope` is a path, not a label — every field present must name a real ancestor of the event.

| Event level | Required scope keys |
|---|---|
| `run.*` | *(none — may be `{}`)* |
| `phase.*` | `phase` |
| `version.*` | `phase`, `version` |
| `step.*`, `gate.*` | `phase`, `version`, `step` |
| `issue.*` | `phase`, `version`, `step`, `issue` |
| `finding.*`, `harden.*` | `phase`, `version` (+ `finding` id in `data`) |
| `tool.*` | *(none — hooks attribute best-effort)* |
| `release.*` | `phase`, `version` |

An event whose scope omits a required key is **malformed** and is quarantined by the reducer (§5.3),
not silently repaired.

---

## 3. Event catalogue

`data` columns list **required** keys; any event may carry extra keys, which consumers ignore.

| Type | status | `data` (required) |
|---|---|---|
| `run.start` | — | `command`, `plan` (ordered version ids), `baseline` `{tests,mypy_errors}`, `git` `{branch,head_sha,remote}`, `resumes?` (prior run id) |
| `run.resumed` | — | `gap_s` — wall-clock idle since the last event |
| `run.estimate` | — | `source` (`estimated`\|`counted`), `versions` `[{id,issues_low,issues_high,points_low,points_high,duration_s}]`, `total`, `rate_basis` |
| `run.end` | ok/fail | `versions_done`, `issues_done` |
| `run.aborted` | fail | `reason` |
| `phase.start` | — | — |
| `phase.end` | ok | `versions` |
| `version.start` | — | — |
| `version.end` | ok | `tag` |
| `version.skipped` | skip | `reason` (`already-released`) |
| `step.start` | — | — |
| `step.end` | ok/fail | — |
| `version.decomposed` | ok | `issues` (ids + `size`) — **the moment scope becomes known**, see §3.2 |
| `issue.uploaded` | ok | `issue`, `gh_number`, `url` |
| `issue.closed` | ok | `issue`, `gh_number` |
| `gate.blocked` | fail | `gate`, `reason` |
| `issue.start` | — | `size` (`S`/`M`/`L`), `area` |
| `issue.implement.end` | ok/fail | `files_changed` |
| `issue.validate.end` | ok/fail | `attempt`, `pytest` `{passed,failed,duration_s}`, `mypy` `{errors}` |
| `issue.commit` | ok | `sha`, `files` |
| `issue.failed` | fail | `attempt`, `reason` |
| `issue.reverted` | fail | `attempt` |
| `issue.end` | ok/fail/skip | `attempts` |
| `finding.raised` | — | `finding`, `severity` (`HIGH`/`MEDIUM`/`LOW`), `title` |
| `finding.classified` | — | `finding`, `disposition` (`fix-now`/`defer`), `home?` |
| `finding.fixed` | ok | `finding`, `sha` |
| `finding.deferred` | skip | `finding`, `home` |
| `harden.start` | — | — |
| `harden.skipped` | skip | `reason` (`--no-harden`) |
| `harden.finding.fixed` | ok | `finding`, `sha` |
| `harden.finding.held` | held | `finding`, `reason` |
| `tool.used` | ok/fail | `tool` — hook-observed; scope is best-effort (§7) |
| `release.tagged` | ok | `tag` |
| `release.pushed` | ok | `tag`, `remote` |

**Pairing rule.** Every `*.start` has exactly one matching `*.end` / `*.skipped` / `*.aborted` in the
same scope. An unmatched `*.start` means the run died mid-node (§9.2).

### 3.1 The estimate, and why it must stay independent

`run.estimate` is emitted once, immediately after the plan is confirmed and **before any version is
decomposed**. It gives the burn-down a total at t=0 and the ETA a value before the first version
finishes — otherwise both are blank for the first several minutes of a run.

`source` distinguishes the two orchestrators, and the difference is real:

- **`/ship-phase` → `estimated`.** Issues do not exist yet, so counts are inferred from each version's
  roadmap Tasks list. Carries genuine uncertainty; the burn-down draws the band.
- **`/ship-solution` → `counted`.** Every planned version already has an issues file (its Step 0.4
  guarantees it), so counts are read, not guessed. `issues_low == issues_high`, and the burn-down has
  **no scope band** — only the time axis is projected.

> **The estimate is never an input to `generate-issues`.** If it were, the decomposer would be told how
> many issues to produce and the comparison would measure nothing but its own suggestion. Estimate and
> actual are *expected* to diverge; keeping them independent is what makes the divergence informative.

**Estimate accuracy** is therefore a derived metric, not an event: at each `version.decomposed` the
reducer compares the real issue count and points against this event's figures, and records signed
error per version plus a run-level bias. A consistent one-directional bias is a finding about the
roadmap or the decomposer — the kind of thing this project exists to surface. It is never used to
correct the estimate mid-run, which would destroy the measurement.

### 3.2 Scope is discovered, not declared

The plan on `run.start` lists **versions**, not issues — because issue counts do not exist yet.
`generate-issues` decomposes one version at a time into 3–7 issues, so a version's issue count is
unknown until its `version.decomposed` event fires, partway through the run.

This is why `version.decomposed` is a first-class event rather than an implementation detail of
`step.end{generate-issues}`: it is the instant total scope changes, and every consumer that shows
progress — burn-down, ETA, "issues done / planned" — must distinguish **known** work from
**estimated** work. A consumer that treats the plan as a fixed issue total will be wrong for most of
the run and will not know it.

Estimating the unknown remainder: for each version not yet decomposed, use the observed mean issue
count so far; before there is one, use the roadmap's 3–7 band. Consumers must carry the low and high
separately (§6 `state.scope`) rather than collapsing to a midpoint.

---

## 4. The log

### 4.1 Layout

```
codegen/runs/<run-id>/events.jsonl     append-only; the source of truth
codegen/runs/<run-id>/state.json       reducer output; disposable, rebuildable
codegen/runs/current                   text file holding the active <run-id>
```

`codegen/runs/` and `codegen/var/` are gitignored. Everything else under `codegen/` is committed.

### 4.2 Append discipline — the concurrency contract

Independent processes append concurrently. The rules that make that safe:

1. Open with `O_WRONLY | O_APPEND | O_CREAT`. **Never** seek; never `r+`.
2. Serialize the event and write it in **exactly one `write()` call**, newline included. Never
   `write(json)` then `write("\n")` — that is two calls and interleaves.
3. **Budget each line to ≤ 4096 bytes.** If `data` would exceed it, truncate the payload (§4.3)
   rather than splitting the line.
4. Close promptly. Do not hold the fd across a long operation.

> **Measured, not assumed.** 12 concurrent processes × 300 lines each, single `write()` per line, on
> macOS/APFS: **0 corrupt lines at 230 B, 430 B, 930 B and 4030 B**. Note `getconf PIPE_BUF` is **512**
> on this platform — the usual "atomic below PIPE_BUF" folklore would have suggested a far tighter
> budget than reality requires. But this is a filesystem-specific observation, not a POSIX guarantee:
> keep the 4 KB budget, keep the single-write rule, and keep the reducer tolerant of a torn final line
> (§5.3) for the crash case, which no atomicity rule can prevent.

### 4.3 Payload truncation

Oversized string values are cut to 512 chars with a `"…"` suffix and the event gains
`data._truncated: true`. Never drop the event; a truncated event is far better than a missing one.

### 4.4 Retention

One directory per run, kept until deleted by hand. `runs/` is gitignored, so growth is a local-disk
concern only. A run worth keeping is promoted by copying it somewhere committed — deliberately, never
automatically.

---

## 5. Emitter

### 5.1 API

```python
def emit(type: str, *, scope: dict | None = None, status: str | None = None,
         data: dict | None = None, emitter: str) -> None:
    """Append one event. Never raises. Never blocks longer than one write()."""
```

CLI form, for hooks and any shell context:

```bash
python3 codegen/tracker/emit.py issue.validate.end \
  --scope version=v01.01,step=execute-issues,issue=RF-003 \
  --status fail --data '{"attempt":1}' --emitter skill:execute-issues
```

### 5.2 The never-raise guarantee

This is the load-bearing property. From vision-doc principle 2: *emission must never gate the
pipeline.* Concretely, `emit` catches **every** exception — disk full, missing directory, unwritable
path, malformed input, absent `runs/current` — and returns normally. On failure it appends one line to
`codegen/var/emit-errors.log` on a best-effort basis and gives up.

A tracker that can fail a build is worse than no tracker.

### 5.3 Reader tolerance

The reducer must survive what the writer cannot prevent:

- **A torn final line** (process killed mid-write) — skip it, count it, continue.
- **A malformed line** (bad JSON, missing required key, unknown `type`) — quarantine to
  `state.json.quarantine[]` with its line number, and continue.
- **An unknown `type`** — retain in quarantine rather than discard; a newer emitter may be writing
  events this reducer predates.

Quarantine counts are surfaced in the dashboard footer. Silent discarding is forbidden — an
observability system that loses data quietly is lying.

---

## 6. Reducer & derived state

```python
def reduce(lines: Iterable[str]) -> State:   # pure; no clock, no filesystem, no network
```

**Purity is the testability lever.** `reduce` takes lines and returns state — no `datetime.now()`, no
env reads. Elapsed time for *open* nodes is computed by the caller, which passes `now` in explicitly.
That is what makes golden-fixture tests (§10.2) possible at all.

`state.json` shape:

```json
{
  "run_id": "run-20260803-142012", "schema": 1, "status": "running",
  "command": "/ship-phase v01", "started": "…", "ended": null,
  "plan": ["v01.01","v01.02","v01.03","v01.04"],
  "tree": [ { "id":"v01.01", "kind":"version", "status":"ok", "start":"…", "end":"…",
              "children":[ {"id":"execute-issues","kind":"step", "…":"…"} ] } ],
  "metrics": { "issues_done": 15, "first_pass_rate": 0.80,
               "mean_issue_s": 302.9, "tests_passing": 156, "findings_open": 2 },
  "scope":   { "known": 17, "est_low": 20, "est_high": 24, "undecomposed": ["v01.04"] },
  "github":  { "created": 17, "closed": 15, "open": 2, "commits": 19,
               "branch": "codegen-tracking", "head_sha": "f069fb6" },
  "eta": { "low_s": 2280, "high_s": 3120, "basis": {"issues_sampled":15,"undecomposed_versions":1} },
  "quarantine": [], "counts": {"events": 412, "torn": 0, "malformed": 0}
}
```

**ETA carries its own basis.** The panel is contractually required to show sample size and how much
scope is undecomposed (vision §6.1), so the reducer emits those fields rather than leaving the UI to
invent confidence. `eta` is `null` until at least one `version.end` exists.

**`scope` is a range, never a scalar.** `known` counts issues from versions already decomposed;
`est_low`/`est_high` add the estimated remainder for versions that are not (§3.2). There is
deliberately no `issues_planned` field — a single number there would be a guess wearing the costume
of a fact, and every consumer would render it as certain.

**`github.commits`** counts every commit the run produced — `issue.commit`, `finding.fixed`,
`harden.finding.fixed`, and the release commits — not just issue commits.

---

## 7. Hook contract

Registered in `.claude/settings.json` — the single permitted file outside `codegen/` (vision §4.1) —
holding only a matcher and a command:

```json
{ "hooks": { "PostToolUse": [ { "matcher": "Bash",
    "hooks": [ { "type": "command", "command": "python3 codegen/hooks/on_tool_use.py" } ] } ] } }
```

Hook scripts receive the harness payload on **stdin** as JSON, and must:

- **Exit 0, always.** A non-zero hook exit can disrupt the session it is observing. Wrap everything.
- **Complete in < 50 ms.** They run on every tool call; a slow hook taxes the whole pipeline.
- **Never print to stdout.** Output may be interpreted by the harness. Diagnostics go to
  `codegen/var/`.
- **Redact before writing** (§8).

Hooks supply the deterministic floor: they observe that `pytest` ran and with what exit code, without
knowing which issue it belonged to. Attribution comes from `runs/current` plus the most recent
`issue.start` — a best-effort join, deliberately, since guessing wrong is better than recording
nothing.

---

## 8. Redaction — the one security requirement

Hooks see **every Bash command**. A command line can contain a model API key. The repo's standing rule
is that secrets live only in the agent's `.env` and are never logged; a naive tracker would break that
rule on day one and write the key to disk in plaintext.

Therefore, before any event is written:

1. **Never record raw command strings.** Record the tool name, argv[0], exit code and duration. If an
   argument sample is genuinely needed, record a hash, not the text.
2. **Redact by pattern** anything resembling a credential — `sk-…`, `ghp_…`, `AKIA…`,
   `*_API_KEY=*`, `Authorization: *` — replacing the value with `«redacted»`.
3. **Never read `.env`**, and never record the environment.
4. **Redaction is the emitter's job, not the caller's** — one implementation, tested (§10.3), so no
   call site can forget.

`codegen/runs/` is gitignored, so a leak would not reach the remote — but "it is only on local disk"
is not a security model.

---

## 9. Failure modes & guarantees

### 9.1 What is guaranteed

- A run that completes normally produces a log whose `*.start`/`*.end` pairs are balanced.
- No emitter failure can fail a pipeline step (§5.2).
- `state.json` is always rebuildable from `events.jsonl`; deleting it loses nothing.
- The log is append-only. Nothing rewrites history.

### 9.2 What is not, and how it surfaces

| Failure | Behaviour |
|---|---|
| Run killed mid-flight | Unmatched `*.start`. A `Stop` hook writes `run.aborted`; if even that is missed, the next orchestrator invocation finds the unterminated run and **asks** whether to resume or supersede it (§9.3). The dashboard marks a run stale after 10 min without events. |
| Skill forgets an emit | Gap is invisible in the log itself — caught by §10.4 reconciliation against hooks. |
| Two runs concurrently | **Unsupported.** `runs/current` is a single pointer. An orchestrator finding an unterminated run treats it as an interrupted run to resume or supersede (§9.3), not as a live one to run beside. |
| Disk full | `emit` fails silently, pipeline continues, `state.counts` stop advancing. |
| Clock skew across emitters | `ts` may go backwards; ordering uses line order (§2.2), so this is cosmetic. |

### 9.3 Resuming an interrupted run

A run that stops without a terminal event is not an error state to refuse — it is the **normal**
outcome of a failed gate, a killed session, or a machine going to sleep. The next orchestrator
invocation detects it and **asks the user** which it is; it never decides alone, because both wrong
answers are costly:

| Choice | Mechanics | Cost of getting it wrong |
|---|---|---|
| **Resume** | Same `run_id`, same log. Append `run.resumed` carrying `gap_s`. | Resuming when a fresh run was meant folds an unrelated session's timings into this one. |
| **New** | `run.aborted` (`reason: "superseded"`) on the old; new run carries `resumes: <old-id>`. | Splitting when resume was meant leaves "how long did v01 take?" unanswerable — the phase spans two runs. |

**Elapsed excludes the gap.** On `run.resumed` the reducer adds `gap_s` to a per-run `idle_s`
accumulator and subtracts it from every elapsed figure. A run paused overnight must not report 14 hours
of velocity — the wall-clock span and the *working* span are different numbers, and every metric here
wants the second one. `state.json` carries both (`elapsed_s`, `idle_s`) so the distinction stays visible
rather than baked away.

`resumes` makes the chain queryable without merging it: cross-run comparison treats linked runs
separately by default and can stitch them on request.

---

## 10. Test strategy

Tests live in **`codegen/tests/`** and run with `pytest`. Type-check with
`mypy --config-file codegen/pyproject.toml codegen/` — the flag is required, because mypy reads config
from the current directory only and would otherwise ignore `codegen/pyproject.toml` and run non-strict. They must not touch the network, must not
call a model, and must not depend on `server/`, `games/`, or `agent/` existing.

> **Isolation is a hard requirement, not hygiene.** The emitter resolves the active run from
> `codegen/runs/current`. A test that forgets to redirect it would append its fixtures to — or worse,
> repoint — **a real run in progress**. So the runs root is never hardcoded: it comes from
> `CODEGEN_RUNS_DIR` (falling back to `codegen/runs/`), an autouse fixture points it at `tmp_path` for
> every test, and a session-scoped guard fails the suite immediately if any test resolves the runs root
> to the real directory.

### 10.1 Schema conformance

- Every event type in §3 has a valid example that validates against `schema.json`.
- For each type, an example **missing each required `data` key** is rejected.
- Scope requirements (§2.3) hold: an `issue.*` event without `version` fails validation.
- Round-trip: `emit` → read back → parse → identical dict.

### 10.2 Reducer — golden fixtures

The core of the suite. Committed fixture logs under `codegen/tests/fixtures/`, each with its expected
state:

| Fixture | Asserts |
|---|---|
| `clean-run.jsonl` | Balanced pairs → complete tree, correct metrics |
| `retry-run.jsonl` | Multiple `issue.validate.end` attempts → `first_pass_rate` correct |
| `aborted-run.jsonl` | Unmatched `version.start` → status `aborted`, no crash |
| `torn-tail.jsonl` | Truncated final line → skipped, `counts.torn == 1` |
| `malformed.jsonl` | Bad JSON + unknown type mid-file → quarantined, rest still reduces |
| `skipped-versions.jsonl` | `version.skipped` → excluded from ETA and velocity |
| `no-review.jsonl` | Version with no review step → **excluded** from findings, not reported as zero |

That last one exists because the prototype got it wrong: it drew a not-yet-reviewed version as a
zero-findings bar, which reads as "clean" when it means "nobody looked." A regression test pins the
distinction.

**Determinism:** reducing the same fixture twice yields byte-identical `state.json`. `reduce` is pure,
so this is cheap to assert and catches accidental clock or environment reads.

### 10.3 Emitter properties

- **Never raises:** parametrized over unwritable path, missing parent dir, non-serializable `data`,
  absent `runs/current`, and a read-only filesystem — every case returns `None` and writes nothing to
  stdout/stderr.
- **Single write:** monkeypatch `os.write` and assert exactly one call per event.
- **Line budget:** a 100 KB `data` payload produces a line ≤ 4096 bytes with `_truncated: true`.
- **Concurrency:** N processes appending M events each yields exactly N×M parseable lines
  (the §4.2 measurement, as an executable test).
- **Redaction:** a table of secret-shaped strings (`sk-ant-…`, `ghp_…`, `AKIA…`, `Authorization:`
  headers) never appears in the output; the redaction marker does.

### 10.4 Reconciliation — testing the untestable part

Skill compliance cannot be unit-tested: whether a model followed an emit instruction is a property of a
*run*, not of code. It is checked **after** a run, as an analysis:

- Every `pytest` invocation seen by hooks has a corresponding skill-emitted `issue.validate.end`.
- Every `issue.commit` has a real commit in `git log`, and vice versa.
- Discrepancies are reported as a **compliance rate**, not a failure. A falling rate is a signal the
  skill files have grown too long — which is the observer-effect risk the vision doc names.

### 10.5 Dashboard

- Reducer output → WS frame shape, asserted against the schema.
- A client connecting mid-run receives a snapshot then a delta stream, and ends in the same state as
  one connected from the start.
- The HTML prototype's palette stays validated: a test shells out to the palette validator and fails
  on a regression, so a colour tweak cannot silently break CVD safety.

### 10.6 Skill instrumentation is verified by real runs, not by a suite

TRK-010–015 change **prompts**, and a prompt has no unit test. There is deliberately no test suite for
them. They are verified two ways, both of which cost nothing extra because they happen anyway:

- **On the next real run** — the log's `*.start`/`*.end` pairs must balance, and the plan the log
  reports must match the plan the skill confirmed. A structural check over a log that a run produced
  regardless.
- **By reconciliation** (§10.4) — a standing measurement of whether the skills emitted what they were
  told to, reported as a rate.

Spinning up throwaway `/ship-phase` runs purely to test instrumentation is not worth it: each costs
~10 minutes and produces real commits, tags and GitHub issues. The runs you were going to do anyway are
the test.

### 10.7 What is deliberately not tested

Visual layout. Screenshots are checked by eye during development (that is how the four prototype bugs
surfaced); pixel-diffing a dashboard against a golden image is a maintenance cost with a poor
detection rate. The palette validator covers the part of "looks right" that is actually computable.

### 10.8 Bridge and firmware

The device design deliberately puts **every computation on the bridge** and leaves the firmware a
renderer, so the hardware-dependent surface is as small as it can be. That is a testability decision
before it is an architectural one: it moves logic from the place that can only be checked by eye into
the place `pytest` already reaches.

| What | Where it runs | How |
|---|---|---|
| `project(state, profile, screen)` | `pytest` | Pure function. Golden frames per profile per screen, over real reduced states from `runs/`. |
| Frame guards | `pytest` | `len(frame) <= 182` and `json.dumps(frame).isascii()` for every frame, including a notification response holding three items. |
| Notification queue, `next`, `dim`, `g` | `pytest` | Volume catalogue, drain order, pacing when a run ends, the brightness ladder, auto-return. No buzzer involved. |
| Poll loop, fan-out | `pytest` | `FakeTransport` records writes. **Two fakes**, so the case that matters is covered: one device dropping must not stall the other. |
| End to end | CI | `bridge/main.py --fake-device` against a real dashboard, no hardware. `tests/replay.py` drives a recorded four-hour run through it in seconds. |
| Frame parser | host C++ | Compiled and tested on the host, no board. Malformed, truncated and future-schema frames each get a case, because all three will happen. |

**Two one-line guards earn their place.** The ASCII assertion means a stray `·` or `–` fails CI rather
than rendering as an empty box on a panel nobody is looking at; the size assertion means a new field
that breaks single-write delivery fails CI rather than silently halving a refresh rate months later.

What genuinely needs hardware: two displays, two buzzers, the LED, and the BLE peripheral itself —
checked by eye, exactly as §10.7 describes. Everything that could be *wrong* rather than *ugly* is
verified in Python.

---

## 11. Schema evolution

`v` is the schema version, currently `1`. Rules:

- **Additive changes** (new event type, new optional `data` key) do not bump `v`. Readers ignore
  unknown keys and quarantine unknown types (§5.3), so old readers survive new writers.
- **Breaking changes** (renaming a field, changing a type, making an optional field required) bump `v`.
  The reducer keeps handling `v-1` for at least one version.
- A log may contain mixed `v` — the tracker can be upgraded mid-run. Reduce per-event by its own `v`.
- `schema.json` is the single source of truth; §2 and §3 of this document are its prose mirror and are
  updated in the same commit as any change to it.

### 11.1 There are two fields named `v`, and they are unrelated

A log event opens `{"v":1,"ts":…}` and a device frame opens `{"v":1,"s":1,…}`. Same key, same current
value, **different contracts on different wires** — and they version independently:

| | Event `v` | Frame `v` |
|---|---|---|
| Governs | `events.jsonl` — this document, §2–3 | the BLE frames — [device-frontends-vision.md](device-frontends-vision.md) |
| Owned by | `tracker/schema.json` | the bridge and the firmware, together |
| Read by | reducer, dashboard, bridge | firmware only |
| Bumps when | the event contract breaks | a frame's shape breaks |

Nothing forces them to move together, and nothing should. The bridge is the only component that sees
both: it consumes events at one version and emits frames at another, which is exactly why the
firmware's `info` characteristic reports the frame version it understands — a bridge speaking frame
`v:2` to firmware that knows only `v:1` says so on the panel instead of rendering nonsense.

Conflating the two would be an easy and expensive mistake: bumping the event schema does not oblige a
reflash, and reshaping a screen does not touch the log.

---

## 12. Build order

Mirrors vision §7; each step is independently useful and none is a prerequisite for the pipeline
itself.

| # | Deliverable | Tests that must land with it |
|---|---|---|
| 1 | `schema.json`, `emit.py`, `runs/` layout | §10.1, §10.3 |
| 2 | `reduce.py` + `state.json` | §10.2 |
| 3 | `ship-phase` spine emits (run/phase/version/step) | fixture from a real run |
| 4 | `execute-issues` emits (issue level, incl. the failure path) | §10.2 retry + no-review fixtures |
| 5 | Hooks + reconciliation | §10.4 |
| 6 | Dashboard server + UI | §10.5 |

Step 4 is where the system starts earning its keep: it is the first point at which the log contains
something git cannot tell you afterwards.

**The device is built after all six**, and against `state.json` rather than against the log — so it
depends on step 2 and nothing later. Its own build order is
[m5-implementation-plan.md](m5-implementation-plan.md); the short version is that everything through
the bridge is ordinary Python needing no purchase, and the firmware stages begin only once that is
green.
