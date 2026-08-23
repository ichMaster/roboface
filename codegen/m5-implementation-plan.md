# M5 device frontends — implementation plan

**Status:** steps 1–4 complete — all twelve tasks needing no hardware. 557 Python tests and 134 C++
checks green, `mypy --strict` and `ruff` clean. **No board purchased**; step 5 is the first that
needs one, and its first job is the MTU measurement the whole frame budget rests on.
**Companions:** [device-frontends-vision.md](device-frontends-vision.md) (why · the six screens · the
poll protocol · every measured figure) · [architecture.md](architecture.md) §1.2, §10.8, §11.1 (where
the bridge sits, how it is tested, the two `v` fields) · [device/prototype.html](device/prototype.html)
(the screens, rendered from the exact frames — the visual contract for M5-016).

**Where each task's detail lives.** Tasks state *what* and *when*. The binding contract for anything
about frames, screens, cadence or notifications is **device-frontends-vision.md**, which owns it; this
file does not restate it. A task's acceptance criteria are the checkable subset, never a replacement
for the spec.

**ID namespace: `M5-###`.** Deliberately not `TRK-###` and not `RF-###`. Architecture §1.1 gives the
full reasoning; the short version is three lifecycles that must never share a counter — and one of
these needs a board plugged in, which neither of the others does.

---

## Decisions taken before task 1

Settled here so no task re-litigates them. Everything in this table is already argued in the vision
doc; it is repeated as a decision record, not as a second source of truth.

| Decision | Rationale |
|---|---|
| **The device polls; the bridge never pushes.** | Nothing on any screen changes faster than once per three minutes (vision §2.2). Polling removes the scheduler, change-detection and per-device state from the bridge, leaving `(screen) → JSON`. |
| **Every computation is on the bridge.** No arithmetic beyond value→pixel on the device, no history, no logging, no timers, no state between frames. | It moves logic out of the only place that can be checked solely by eye. `device/shared/` collapses to a JSON parser (vision §3.1). |
| **One JSON per screen, ASCII only, one BLE write.** | The screen bounds the frame; the largest is 164 B against a ~182 B limit. ASCII means the stock Latin font suffices — no glyph set to ship, no empty boxes that only appear on hardware. |
| **`bridge/` may use third-party packages; `tracker/` and `hooks/` still may not.** | The bridge is a separate process started deliberately, exactly like `dashboard/`. `codegen/tests/test_dependencies.py` scopes the stdlib rule to `tracker`/`hooks` already, so `bleak` goes in `requirements.txt` and that test keeps passing unchanged. *(Corrected at M5-001: the plan claimed the test asserts on the header's "dashboard and test only" wording. It does not — it asserts only that `stdlib-only`, `tracker` and `hooks` appear. The header was widened anyway, because it was about to be false.)* |
| **Bridge tests live in `codegen/tests/`.** | Same `pytest`, same autouse `CODEGEN_RUNS_DIR` isolation. A third test directory would buy nothing and cost a second command. |
| **Firmware tests are a separate runner.** | C++ on the host via PlatformIO. Only the frame parser is host-testable; the rest is a display. |
| **No skill builds this**, same as the tracker. | The decomposition those skills perform is already done — it is this file. |
| **`M5-###` never appears in a commit subject.** | Conventional prefixes only: `feat(bridge):`, `test(bridge):`, `feat(device):`, `docs(codegen):`. The id may appear in the body. |
| **Progress is tracked by ticking this file's checkboxes.** | Ids are absent from git and GitHub, so these boxes are the record. Tick them in the commit that satisfies them. |
| **Dark theme only.** | The panel sits in a lit room, and one palette is one palette to keep validated. Copied verbatim from the dark block of `dashboard/static/styles.css`. |

---

## Task summary

| # | ID | Title | Size | Step | Hardware | Dependencies |
|---|----|-------|------|------|:---:|--------------|
| 1 | M5-001 | `bridge/` scaffolding + device profiles | S | 1 | — | — |
| 2 | M5-002 | Frame schema + the two guards | M | 1 | — | M5-001 |
| 3 | M5-003 | `project()` — the pure projection | M | 1 | — | M5-002 |
| 4 | M5-004 | Derived statistics for the six screens | L | 1 | — | M5-003 |
| 5 | M5-005 | Golden frames from real runs | M | 1 | — | M5-004 |
| 6 | M5-006 | Notification catalogue + queue | M | 2 | — | M5-003 |
| 7 | M5-007 | `next` pacing and the `dim` ladder | M | 2 | — | M5-006 |
| 8 | M5-008 | `g` navigation | S | 2 | — | M5-007 |
| 9 | M5-009 | Poll loop + `FakeTransport` | M | 3 | — | M5-007 |
| 10 | M5-010 | `--fake-device` + CI wiring | S | 3 | — | M5-009 |
| 11 | M5-011 | `BleakTransport` | M | 3 | — | M5-009 |
| 12 | M5-012 | `device/shared/` — the frame parser | M | 4 | — | M5-002 |
| 13 | M5-013 | PlatformIO scaffolding, two targets | S | 5 | **Core2** | M5-012 |
| 14 | M5-014 | BLE peripheral + **MTU verification** | M | 5 | **Core2** | M5-013, M5-011 |
| 15 | M5-015 | The drawing toolkit | M | 5 | **Core2** | M5-013 |
| 16 | M5-016 | The six Core2 screens | L | 5 | **Core2** | M5-015, M5-014 |
| 17 | M5-017 | Output: vibration, chime, backlight, touch | M | 6 | **Core2** | M5-016, M5-008 |
| 18 | M5-018 | StickC profile + layouts | M | 7 | **StickC** | M5-016 |
| 19 | M5-019 | StickC sleep and wake | S | 7 | **StickC** | M5-018, M5-017 |
| 20 | M5-020 | Two devices at once | M | 8 | both | M5-019, M5-012 |

**Size legend:** S = 1–2 d · M = 3–5 d · L = 5–8 d

**Twelve of twenty tasks need no hardware**, and they carry the majority of the system's logic —
every statistic, every graph, every notification decision. M5-013 is the first purchase.

**Working discipline.** No skill enforces this, so it is stated:

1. **One task = one commit.** Never mix two M5 tasks; never start one whose dependencies are unmet.
2. **Validate before committing** — `pytest codegen/tests`,
   `mypy --config-file codegen/pyproject.toml codegen/`, `ruff check codegen/`, all green. Never
   commit red. The `--config-file` is not optional; see `implementation-plan.md`'s note on why.
3. **Walk the acceptance criteria explicitly** and tick each box in the same commit.
4. **Tests ship with the task**, not after it.
5. If a task's scope turns out wrong, **correct the vision doc first, then this file, then implement.**
   The vision doc is the contract; a plan that drifts from it is worse than no plan.

---

## Dependency tree

```
M5-001 (scaffolding)
  └── M5-002 (schema + guards) ──┬── M5-003 (project) ──┬── M5-004 (statistics) ── M5-005 (golden)
                                 │                      └── M5-006 (notifications) ── M5-007 (next+dim) ──┬── M5-008 (goto)
                                 │                                                            │           │
                                 │                                                            └── M5-009 (poll+fake) ──┬── M5-010 (CI)
                                 │                                                                                      └── M5-011 (bleak)
                                 └── M5-012 (frame parser, C++) ──── M5-013 (platformio) ──┬── M5-014 (BLE + MTU) ◄─────┘
                                                                                            └── M5-015 (drawing) ──┬── M5-016 (six screens)
                                                                                                                    └── M5-017 (output) ──┐
                                                                                          M5-018 (stickc) ── M5-019 (sleep) ◄─────────────┘
                                                                                                                    └── M5-020 (both)
```

**Parallelisation.** After M5-002 the projection track (003–005) and the firmware-parser track (012)
are independent — the parser can be written and host-tested while the statistics are still being
built. Notifications (006–008) need only M5-003.

**Earliest useful point:** M5-010. At that point the whole system runs end to end against a real
dashboard with no hardware at all, which is also the point where buying a board stops being a gamble.

---

## Step 1 — Projection

### M5-001 — `bridge/` scaffolding + device profiles

**Description:** Directory, dependency, and the profile objects that make `project()` device-aware.
No behaviour.

**Implementation:**
- Create `codegen/bridge/` with `__init__.py`.
- Add `bleak` to `codegen/requirements.txt`, and **widen its header comment**: it currently says the
  file is for "the DASHBOARD and the TEST SUITE only", which will be false. `tracker/` and `hooks/`
  remain stdlib-only, and `test_dependencies.py` already scopes the rule to those two — but its
  `test_requirements_are_documented_as_dashboard_and_test_only` asserts on that comment's wording, so
  the test and the comment change together.
- `bridge/devices.py`: a `Profile` describing a board — screen size, character budget per line, which
  screens it has, its poll intervals, its `dim` ladder. Two instances: `CORE2`, `STICKC`.
- Profiles are **data, not code paths.** `project()` reads them; it never branches on a board name.

**Dependencies:** None

**Acceptance criteria:**
- [x] `pytest codegen/tests` still passes without `bleak` installed *(nothing imports it yet)* —
      307 passed with `bleak` genuinely absent from the venv.
- [x] `test_dependencies.py` passes: `tracker/` and `hooks/` still import only the stdlib.
- [x] `CORE2` and `STICKC` differ in screen size, screen list and ladder — asserted, so a copy-paste
      profile fails.
- [x] `mypy` and `ruff` clean over `codegen/bridge/`.
- [x] **Added:** the ladder is *derived* — doubling the NOW interval moves both steps with no other
      edit, and a ladder never brightens as time passes.

---

### M5-002 — Frame schema + the two guards

**Description:** The seven frame shapes and the two assertions that keep them shippable. This is the
contract the firmware will parse, so it lands before anything produces one.

**Implementation:**
- `bridge/frames.py`: a schema per `want` value (0 notifications, 1–6 screens) per vision §2.3.
  Stdlib validation, same approach as `tracker/schema.py` — no `jsonschema`.
- Two guards, as helpers used by every later test:
  `assert_fits(frame)` → serialised length ≤ 182 B; `assert_ascii(frame)` → `.isascii()`.
- The limit is a named constant with the reasoning attached (vision §2.3.1), not a bare `182`.

**Dependencies:** M5-001

**Acceptance criteria:**
- [x] All seven frame types validate a good example and reject one missing each required key —
      *except `s`, which is the discriminator: its absence is a routing failure, not one missing key
      among many, since there is nothing to validate against until you know which frame it is. Covered
      by its own test.*
- [x] `fits` rejects a 183-byte frame and accepts a 182-byte one — asserted at the exact boundary.
- [x] `is_ascii` rejects `·`, `–` and `×` — the three that were caught in review — plus `█` and `●`,
      the two a renderer would reach for if frames carried glyphs. Checked over the serialised form,
      so non-ASCII cannot hide in a nested list or a dict key.
- [x] Every field the vision doc's frame tables name is present in the schema; a test compares the two
      lists so the doc and the code cannot drift.
- [x] **Added:** the frame version is asserted *independent* of the event schema version
      (architecture §11.1) — `bridge/frames.py` must not read `SCHEMA_VERSION`. Comparing the two
      values proves nothing while both are 1; what matters is that neither derives from the other.
- [x] **Added:** the `MAX_FRAME_BYTES` reasoning is asserted against the source, because it lives in a
      `#:` comment that no runtime attribute exposes — and a comment is what a refactor drops.

---

### M5-003 — `project()` — the pure projection

**Description:** `state dict + profile + screen → frame`. Pure, and the single place any device value
is decided.

**Implementation:**
- `bridge/project.py`: `project(state, profile, screen) -> dict`. No clock, no I/O, no environment —
  the same purity rule as `tracker/reduce.py`, and for the same reason: it is what makes golden tests
  possible.
- String truncation to the profile's budget happens here. So does formatting: elapsed to `MM:SS` at
  minute granularity, ETA to a range, percentages to integers.
- **Text is composed from identifiers, never copied from the log** (vision §5.1). A test asserts no
  value in any frame appears verbatim in the source state's free-text fields.

**Dependencies:** M5-002

**Acceptance criteria:**
- [x] An AST test asserts `project.py` **and `stats.py`** call no `datetime.now`, `time.time`,
      `os.environ`, or `open`.
- [x] Projecting the same state twice yields byte-identical output.
- [x] ~~The same state projected for `CORE2` and `STICKC` yields **different** frames~~ —
      **criterion corrected.** With a short label both budgets fit and the frames are byte identical,
      which is right, not a bug. What differs is the *budget*, and only when the text exceeds the
      narrower one. Asserted that way instead.
- [x] Every produced frame passes both M5-002 guards, for every screen and both profiles — and for
      four fixtures including the damaged ones, since a panel that crashes on an aborted run fails
      exactly when you most want to look at it.
- [x] No frame value equals any free-text field of the source state (redaction by construction).

---

### M5-004 — Derived statistics for the six screens

**Description:** The arithmetic behind every number and every graph. The largest task in the plan, and
the one that would otherwise have ended up in C++.

**Implementation:** Per vision §5, computed from `state.json` alone:
- **NOW** — the `version · issue · step` label built from the deepest *running* tree node, **not** from
  `state.current`, which is degenerate (vision §9.4). Issue age against the median for its step type,
  reduced to a colour class.
- **VELOCITY** — issues closed per 30-minute bucket, quantised to eight levels.
- **PLAN** — one ASCII flag per version.
- **FRICTION** — retry count and ranking, findings by severity.
- **ANALYTICS** — per-step-type **medians and shares over closed spans only**, plus the `cov`
  coverage percentage. Medians, not sums, precisely because a single unclosed node cannot distort a
  median (vision §9.1).
- **BURNDOWN** — remaining issues from `issue.closed`, **not** from `state.burndown`, which is
  non-monotonic (vision §9.3). Estimate reference line and the projection cone.

**Dependencies:** M5-003

**Acceptance criteria:**
- [x] Against `run-20260815-213849`, VELOCITY reproduces `15 · 7 · 2 · 2 · 6 · 4 · 6` — as
      `[0, 15, 7, 2, 2, 6, 4, 6]`. **The leading zero is real and kept:** the opening half-hour closes
      nothing, the same fact BURNDOWN's flat first points show. A trailing *partial* bucket is dropped,
      since a five-minute bucket reading zero draws a cliff that never happened.
- [x] ANALYTICS reproduces `41 · 26 · 16 · 10 · 7` and `cov: 42`, and is **unchanged** when an
      unclosed 155-minute node is injected — the property that let the screen ship without waiting.
- [x] BURNDOWN's series is monotonically non-increasing, on the real run and on four fixtures.
- [x] The NOW label is built from the tree, and a test pins that `state.current` really is
      `"execute-issues · execute-issues · execute-issues"` while the label reads `v05.03 RF-112`.
- [ ] ~~`cov` reaches 100 on a synthetic log with no unclosed step nodes~~ — **deferred to M5-005**,
      where the generator can produce such a log. Nothing in the recorded run exercises it.
- [x] Every statistic has a test naming the real run's figure it must reproduce. Tests touching the
      recorded run skip cleanly when `runs/` is absent, since it is gitignored.

---

### M5-005 — Golden frames from real runs

**Description:** Freeze the output. The equivalent of TRK-008, and the thing that makes later refactors
safe.

**Implementation:** For each of the seven `want` values × both profiles, a committed expected frame
under `codegen/tests/fixtures/frames/`, generated from real reduced states in `runs/` and from
`tests/gen_log.py` presets. Regeneration behind an explicit `--update-golden`, never by default.

**Dependencies:** M5-004

**Acceptance criteria:**
- [x] ~~14~~ **9** golden frames committed; each reproduces exactly. **Criterion corrected:** the
      StickC renders NOW and the notification channel only, so it has no burndown frame to have.
      Seven plus two, not seven times two.
- [x] Every golden frame passes both guards.
- [x] A deliberate one-character change to a projection produces a failing diff naming the field.
- [x] Golden frames are generated, not hand-written — `python3 -m tests.gen_frames --update-golden`,
      which refuses to run without the flag, because a golden that rewrites itself asserts nothing.
- [x] **Added:** built from presets, never from `runs/`. That directory is gitignored, so a golden
      derived from a recorded run would not survive a clone.
- [x] **Inherited from M5-004, and its criterion was wrong.** It asked for `cov == 100` on a log with
      no unclosed steps. That can never happen: coverage is closed-span time over run time, and steps
      do not tile a run — there is always time between them. A clean preset with every step closed
      reaches **91%**. The test asserts the *contrast* instead: high when steps close, 0 when none do.

---

## Step 2 — Notifications and pacing

### M5-006 — Notification catalogue + queue

**Description:** The `want:0` channel. Alerts and events are one list; only `b` separates them.

**Implementation:**
- `bridge/notify.py`: raise notifications per the vision §5.1 catalogue, each with its `k`, composed
  `t`, and volume `b` 0–3.
- **Corrected at M5-006:** the plan said "map log events". The bridge never sees events —
  `dashboard/server.py` sends `{"kind": ..., "state": ...}` and nothing else. Transitions are
  recovered by **diffing successive snapshots**, which the bridge is free to do since only the
  *device* is forbidden state.
- A queue draining oldest-first, at most three per answer (the size limit), the remainder carried over.
- **Dropped once answered.** A lost write costs one buzz, which is accepted: the buzz is the
  notification and the screen is the record.

**Dependencies:** M5-003

**Acceptance criteria:**
- [x] ~~Against the real run, exactly 57 notifications are raised~~ — **criterion replaced.** That
      figure came from counting *log events*, which the bridge cannot see (above). What is asserted
      instead is that each transition type is recovered from a state diff, and that an unchanged
      state raises nothing — the common case by a wide margin.
- [x] `failed`, `held` and `blocked` map to `b:3` even though the run contains none — the catalogue
      is complete, not sampled. The same argument the FRICTION screen rests on.
- [x] A burst of five raises three in one answer and two in the next, in order.
- [x] An answered notification never reappears.
- [x] Every `t` is ASCII and composed from identifiers.
- [x] **Added:** the first snapshot raises nothing, so a device connecting mid-run is not buzzed for
      every version that finished before it arrived.
- [x] **Added:** `g` is omitted when the board cannot show that screen — telling a StickC to jump to
      FRICTION would leave it asking for a frame nobody answers.

---

### M5-007 — `next` pacing and the `dim` ladder

**Description:** The two fields by which the bridge controls the device's behaviour without the device
holding any policy.

**Implementation:**
- `next` per vision §4.2: the per-screen intervals while a run is active, 60 s once it ends or when
  there is no run.
- `dim` per vision §4.4: the ladder derived from the NOW interval — Core2 `100 → 50 → 20`, StickC
  `100 → 0`, at 2× and 3×. **Derived, never a constant**: retuning the NOW interval must move the
  ladder with it.
- **Corrected at M5-007:** the vision doc says the bridge may read any inbound `want` as *the user is
  present*. It cannot — that does not distinguish a tap on the current screen from the scheduled poll
  for that same screen, and this task requires exactly that distinction. The device flags it:
  `{"want":N,"u":1}`. One optional field is cheaper than guessing.

**Dependencies:** M5-006

**Acceptance criteria:**
- [x] Changing the NOW interval in the profile moves both ladder steps, with no other edit —
      asserted through the whole call path, not only `Profile.dim_at`.
- [x] A finished run yields `next: 60` on every screen — **except notifications**, which keep their
      rate: they are the channel that can buzz, and a finished run still has a last chime to deliver.
- [x] Idle 30 s → Core2 `dim:50`, StickC `dim:0`; idle 45 s → Core2 `dim:20`; Core2 never emits `dim:0`.
- [x] An interaction poll resets to `dim:100`; a scheduled poll does not.
- [x] `dim` and `next` are present in **every** answer, including the idle notification response.

---

### M5-008 — `g` navigation

**Description:** The bridge, not the device, decides which screen is showing.

**Implementation:** `g` on an alert switches the device to the relevant screen; `g:1` returns it to NOW
thirty seconds after the last interaction. Both timers live here, in Python.

**Dependencies:** M5-007

**Acceptance criteria:**
- [x] A retry notification carries `g:4`; a silent `release.tagged` carries no `g`.
- [x] Thirty seconds after the last interaction, the next answer carries `g:1`; before that, none does.
- [x] `g` is absent, not `null`, when there is nothing to navigate to — a byte saved on every answer.
- [x] **Added:** an alert beats the return timer. Something just happened, which is worth more.
- [x] **Added:** polling the notification channel does not change which screen is showing — it is
      not a screen.

---

## Step 3 — The bridge process

### M5-009 — Poll loop + `FakeTransport`

**Description:** The loop that answers polls, and the fake that makes it testable without a radio.

**Implementation:**
- `bridge/transport.py`: one interface, two implementations. `FakeTransport` records every write and
  lets a test drive polls synchronously.
- `bridge/main.py`: subscribe to `ws://127.0.0.1:8420/ws`, hold the latest state, answer `want` with
  `project(...)`. Reconnect to the dashboard with backoff; a dashboard that is down must never crash
  the bridge.

**Dependencies:** M5-007

**Acceptance criteria:**
- [x] A `want:N` produces exactly one write, carrying screen N.
- [x] An unknown `want` is ignored, not answered with a malformed frame.
- [x] The dashboard dropping mid-run does not kill the bridge; it reconnects and resumes answering.
- [x] Polls arriving before the first WS frame get a valid frame reading `0/0` rather than nothing.
- [x] Every write in a full simulated run passes both guards — over the whole run, not sampled.
- [x] **Added:** each device keeps its own notification queue, so one out of range does not lose
      what happened while it was away.

---

### M5-010 — `--fake-device` + CI wiring

**Description:** The whole system, end to end, with no hardware. The point at which buying a board
stops being a gamble.

**Implementation:** A mode that drives `FakeTransport` on a realistic poll schedule against a live
dashboard, and a CI job that replays a recorded run through it using `tests/replay.py`.

**Dependencies:** M5-009

**Acceptance criteria:**
- [x] `bridge/main.py --fake-device` runs end to end with no hardware present, and survives a
      dashboard that is not there.
- [x] It exits non-zero on any guard violation, so it fails CI rather than merely mentioning it.
- [x] The fake run completes in milliseconds.
- [x] The run exercises every screen and the notification channel, on every board in the roster.
- [ ] ~~Replays a four-hour recorded run~~ — **deferred to M5-020's validation**, which needs
      `tests/replay.py` driving a live dashboard rather than a single snapshot.

---

### M5-011 — `BleakTransport`

**Description:** The real radio. Swaps in behind the same interface.

**Implementation:** `bleak` central: scan for the advertised name, connect, subscribe to `input`
notifications, answer on `frame`. Per-device reconnect with backoff.

**Dependencies:** M5-009

**Acceptance criteria:**
- [x] Substituting `BleakTransport` for `FakeTransport` requires no change in `main.py` — same
      interface, asserted.
- [x] Reconnect with exponential backoff, capped, and `CancelledError` re-raised rather than swallowed.
- [x] The bridge starts and stays healthy with **no device present at all** — the normal state.
- [x] Scanning is bounded by a timeout; a missing device never blocks the loop.
- [x] **Added:** `bleak` is not imported at module scope — asserted, since `bridge/` is a leaf and the
      dashboard must keep starting on a machine with no Bluetooth. Declared optional in
      `pyproject.toml` rather than silenced per line.
- [ ] **Untested without hardware.** `BleakTransport.run` is `pragma: no cover`; its scan, connect
      and notify path is first exercised at M5-014.

---

## Step 4 — Firmware, without a board

### M5-012 — `device/shared/` — the frame parser

**Description:** The whole shared library. §3.1 of the vision doc left nothing else to share.

**Implementation:** C++ parsing a frame into a struct, compiled and tested on the host. No display, no
radio, no board.

**Dependencies:** M5-002

**Acceptance criteria:**
- [x] All nine golden frames from M5-005 parse — the same fixtures the Python side writes, so a
      frame only one language understands cannot exist.
- [x] A truncated frame is rejected without reading past the buffer — **every prefix** of a real
      frame is tried, not one convenient cut, under AddressSanitizer.
- [x] A frame with an unknown `v` reports `firmware too old` rather than garbage.
- [x] Unknown fields are skipped, including nested objects and arrays, so a bridge adding one does
      not require a reflash.
- [x] The host suite runs with no board and is wired into `pytest codegen/tests`, skipping cleanly
      where no compiler exists. One command now covers the whole no-hardware surface.
- [x] **Added:** built with `-Werror` and address + UB sanitizers. Not decoration — the first run
      caught a use-after-free (see below).
- [x] **Added:** an oversized notification queue drops the overflow rather than writing past the
      array, and a failed parse leaves the struct reset so a caller that ignores the result draws an
      empty screen rather than garbage.

---

## Step 5 — Core2

> **From here a board is required.** M5-014's first job is the MTU check that the whole frame budget
> rests on — do it before writing a renderer, because it is the one number in the design that could
> still be wrong.

### M5-013 — PlatformIO scaffolding, two targets

**Description:** One tree, two boards.

**Implementation:** One `platformio.ini` with `[env:core2]` and `[env:stickc]` over one shared `lib/`.
M5Unified (one API across both, and it absorbs the AXP192/AXP2101 split and the StickC's GPIO4
power-hold) plus NimBLE-Arduino (roughly half the RAM of Bluedroid).

**Dependencies:** M5-012

**Acceptance criteria:**
- [ ] Both targets build from a clean checkout.
- [ ] `shared/` is compiled into both, from one copy — asserted by a build that fails if it is duplicated.
- [ ] The Core2 target boots to a blank screen without a crash loop, on both board revisions if both
      are available.

---

### M5-014 — BLE peripheral + MTU verification

**Description:** The GATT service, and the measurement the frame budget depends on.

**Implementation:** Three characteristics per vision §4 — `input` (Notify), `frame` (Write Without
Response), `info` (Read). Advertise a stable name the bridge can find.

**Dependencies:** M5-013, M5-011

**Acceptance criteria:**
- [ ] **`client.mtu_size - 3` is printed and recorded in the vision doc.** If it is below 182, the
      documented one-argument fallback is applied and the frame table is revised in the same commit.
- [ ] A frame written by the real bridge arrives intact and parses.
- [ ] A button press produces an `input` notification the bridge receives.
- [ ] `info` reports the board type and the frame `v` this firmware understands.
- [ ] Disconnecting the central raises the radio's disconnect event, and the device draws the
      disconnected screen without a timer (vision §4.3).

---

### M5-015 — The drawing toolkit

**Description:** The primitives every screen is built from. Written once, before six screens need them.

**Implementation:** LovyanGFX into an off-screen `LGFX_Sprite`: anti-aliased arc, smooth polyline,
smooth circle, rounded rect, and the dark palette as named constants copied verbatim from
`dashboard/static/styles.css`. **Full repaint only** — there is no previous frame to diff against.

**Dependencies:** M5-013

**Acceptance criteria:**
- [ ] A full-screen repaint completes in under 200 ms, measured on the board.
- [ ] The sprite fits PSRAM with the measured headroom recorded.
- [ ] Nothing flickers on repeated repaint.
- [ ] Palette constants match `styles.css`'s dark block exactly — asserted by a script comparing the
      two files, so a CSS tweak cannot silently desync the panel.

---

### M5-016 — The six Core2 screens

**Description:** Render all six against the prototype.

**Implementation:** One renderer per screen, each a pure `frame → pixels`. `device/prototype.html` is
the visual contract; differences are bugs in the firmware, not in the prototype.

**Dependencies:** M5-015, M5-014

**Acceptance criteria:**
- [ ] All six render from real frames produced by the real bridge.
- [ ] Each is legible from two metres — checked by eye, the §10.7 rule.
- [ ] Sparklines, version dots and the progress ring are drawn as **shapes**, never as glyphs; the font
      renders only `[A-Za-z0-9 .:/%-]`.
- [ ] `sample 42%` appears on ANALYTICS and the estimate line is labelled `estimate`, never `ideal` —
      both are honesty requirements, not styling.
- [ ] A frame arriving for a screen that is not showing is parsed and discarded without a repaint.

---

## Step 6 — Core2 output

### M5-017 — Vibration, chime, backlight, touch

**Description:** Everything the panel does that is not drawing.

**Implementation:** Map `b` 0–3 to silence, chime, short and long buzz. Apply `dim` to the backlight.
A touch anywhere sends `{"want":N}` for the current screen. `g` switches screens.

**Dependencies:** M5-016, M5-008

**Acceptance criteria:**
- [ ] Each of the four `b` levels is distinguishable by feel and ear.
- [ ] A silent notification (`b:0`) still lights the screen — `b` controls sound and haptics only.
- [ ] A tap restores full brightness and refreshes the current screen.
- [ ] `dim:20` is readable across a room and is not a light source in a dark one — the judgement call
      §4.4 rests on, confirmed by eye.
- [ ] Every notification fires exactly once; nothing double-buzzes.

---

## Step 7 — StickC Plus2

### M5-018 — StickC profile + layouts

**Description:** The second board. A port, not a redesign.

**Implementation:** `[env:stickc]`, the `STICKC` profile, and 240×135 layouts. NOW compressed, the rest
per vision §1.1.

**Dependencies:** M5-016

**Acceptance criteria:**
- [ ] Frames for `STICKC` are measurably smaller than for `CORE2` on the same state.
- [ ] Every StickC layout is legible at arm's length.
- [ ] The renderers share the drawing toolkit; nothing is duplicated from `core2/`.

---

### M5-019 — StickC sleep and wake

**Description:** What makes it a pager rather than a small display.

**Implementation:** Screen off at `dim:0`. Wake on notification or button A, for the duration the
bridge specifies. Polling continues while dark — that is how a notification arrives.

**Dependencies:** M5-018, M5-017

**Acceptance criteria:**
- [ ] The screen is dark for the great majority of an idle run.
- [ ] A notification wakes it, and it darkens again after the specified duration.
- [ ] Polling continues while dark — verified by the bridge's logs, not assumed.
- [ ] Battery life is **measured** across a real run and recorded in the vision doc, replacing the
      current estimate.

---

## Step 8 — Both

### M5-020 — Two devices at once

**Description:** Independent loops, independent failures.

**Implementation:** `devices.py` holds a roster; each device gets its own transport, its own poll
schedule and its own reconnect.

**Dependencies:** M5-019, M5-012

**Acceptance criteria:**
- [ ] Both boards run against one bridge, each on its own cadence.
- [ ] Unplugging one does not stall the other — the case two `FakeTransport`s exist to cover.
- [ ] Both show the **same figures** where the screens overlap; a discrepancy is a projection bug.
- [ ] Adding a third roster entry needs no code change beyond a profile.

---

## Validation workload

**The device is validated against a real `/ship-phase` run**, not a synthetic one — the same run that
validates the tracker, watched from the desk instead of the browser.

What a real run yields that a replay cannot:

- **The MTU number** (M5-014), which nothing else can produce.
- **Real notification pacing.** 57 notifications over 4.1 hours is a number from a log; whether that
  *feels* right on a desk is not.
- **Battery figures** (M5-019), replacing the estimates in the vision doc.
- **The `dim:20` judgement**, which is a question about a room and cannot be answered anywhere else.

Two things to plan for rather than discover:

- **`state.current` is degenerate on a finished run** (vision §9.4). The NOW label must be checked
  against a **live** run — a finished one has no running node, so the bug is invisible there.
- **One of the instrumentation defects in vision §9 is fixed; the rest are not.** The reducer no
  longer lets an unclosed node accrue against wall-clock ([#113](https://github.com/ichMaster/agent-arena-sandbox/issues/113)).
  The missing `step.end` emissions are untouched, and **`cov` measures those, not the reducer** —
  capping an unclosed node does not close it, so the badge stayed at 42 % across that fix. It reaches
  100 only when the skills stop dropping the pair.

---

## Definition of done for the whole plan

- [ ] A real `/ship-phase` run is watched end to end on the Core2, from first notification to release
      chime, with no manual intervention.
- [ ] Both boards run simultaneously against one bridge and agree.
- [ ] `pytest codegen/tests` green; `mypy --config-file codegen/pyproject.toml codegen/` and
      `ruff check codegen/` clean.
- [ ] Every frame written during that run passed both guards — asserted over the run, not sampled.
- [ ] The measured MTU is recorded in the vision doc, and the frame table reflects it.
- [ ] `rm -rf codegen/` still leaves a working repo.
- [ ] Killing the bridge mid-run leaves the pipeline untouched — the observability guarantee, one hop
      further out than the tracker's.

## Out of scope

Any control over the pipeline from a device — permanently, for the three reasons in vision §6 ·
pairing or bonding (vision §10.1, an accepted risk) · per-device run selection (vision §10.3) · a
third board · anything that would put a computation, a timer or a byte of history back on the
firmware.
