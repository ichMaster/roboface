# Physical bodies for the dashboard — Core2 and StickC Plus2 over BLE

**Status:** design vision. Nothing is built yet, but the screens are
[prototyped](device/prototype.html) and every figure below is measured, not assumed.
**Scope:** two physical, glanceable rendering surfaces for a `/ship-phase` run, fed from the laptop
over Bluetooth Low Energy. An *addition* to the browser dashboard, never a replacement.

Companion documents: [ship-phase-tracking-vision.md](ship-phase-tracking-vision.md) (why tracking
exists), [architecture.md](architecture.md) (the event log and reducer),
[dashboard-specification.md](dashboard-specification.md) (the web UI this projects from),
[device/prototype.html](device/prototype.html) (the six screens, rendered from the real frames),
[m5-implementation-plan.md](m5-implementation-plan.md) (the 20 `M5-###` tasks that build this).

---

## 1. The problem this solves

The run measured throughout this document — the full roadmap, v01.01 through v05.03.00 — took
**4.1 hours** (`elapsed_s: 14702`, 1701 events, 42 issues, 15 releases). That is the normal shape of
a run: long, mostly autonomous, punctuated by a handful of moments that want a human.

The browser dashboard shows all of it, beautifully, **in a tab you are not looking at.** Over four
hours you will not keep it foregrounded, and a background tab cannot get your attention. The failure
mode is not "I could not see the run" — it is "the run needed me nine minutes ago and I was in a
different window."

A physical device fixes that and nothing else. It is **always visible without being focused**, it can
**buzz**, and it **cannot be covered by another window**. The Core2 dims when left alone but never
goes dark (§4.4) — precisely so that first claim keeps being true.

> **Design consequence, stated up front:** these are *ambient annunciators*, not small dashboards.
> When a feature would be better on the laptop, it goes on the laptop. §6 gives the two rules that
> keep six screens compatible with that claim.

## 1.1 Two devices, two jobs

|  | **M5Stack Core2** | **M5StickC Plus2** |
|---|---|---|
| **Role** | desk display — *what is it doing?* | pager — *does it need me?* |
| **MCU** | ESP32-D0WDQ6-V3 | ESP32-PICO-V3-02 |
| **Screen** | 320×240 IPS, capacitive touch | 135×240 TFT, no touch (42% of the area) |
| **Alerts** | vibration motor + speaker | buzzer + red LED |
| **Battery** | 390 mAh | 200 mAh |
| **Power plan** | USB-C on the desk, dims to 20% but never dark | battery, screen sleeps between notifications |
| **Screens** | six (§5) | NOW, compressed |

The Core2 sits on the desk and is **read**. The StickC clips to a pocket and is **felt** — it sleeps,
then beeps and lights up when something needs you, so you can walk away during a four-hour run
without losing the thread.

Both are ESP32-class, both are supported by **M5Unified**, and both speak the same BLE stack, so the
second board costs a rendering layer rather than a second system.

---

## 2. Four measurements that decide the architecture

Nothing here is a preference. Each constraint below was measured against the real run, and each one
forces a specific decision.

### 2.1 The state object is 24 KB — so a projection is mandatory

Reducing the run yields the object the browser receives on every frame:

```
FULL state                24,469 bytes
  tree                    17,359   (71%)
  burndown                 3,336
  estimate                 1,905
  findings                   877
  everything else          ~1,000
```

BLE moves data in MTU-sized chunks, and 24 KB is far past any single write. The state object cannot
go to the device; a **projection** must.

### 2.2 Nothing changes faster than every three minutes — so the device polls

Measured intervals between actual changes to each screen's content:

| Screen | Changes every | Driven by |
|---|---|---|
| NOW | 3 min | a new issue or step |
| BURNDOWN | 6 min | an issue closing |
| ANALYTICS | 7 min | a step completing |
| PLAN | 8 min | a version starting or ending |
| FRICTION | 12 min | a retry or a finding |
| VELOCITY | 31 min | the 30-minute buckets |

This is not a real-time system, and the numbers say so plainly. **So the device asks, rather than the
bridge announcing.** The device polls for what it is currently displaying; the bridge answers. That
choice removes, from the bridge, a scheduler, change-detection, and any record of what each device is
showing — it becomes a function of `(screen number) → JSON`. And it removes from the firmware every
reason to hold state (§3.1).

Because a peripheral cannot read from a central, a poll is **Notify out, Write back**:

```
device --Notify--> {"want":4}          10 B
bridge --Write --> {…screen 4 JSON…}   71-164 B
```

**A button press is the same message.** Pressing a button changes which screen the device asks for
and sends the request immediately. There is no separate button protocol, and no waiting for the next
interval when you switch.

### 2.3 One frame per screen — and the screen keeps it small

Each screen gets its own JSON, prepared on the bridge:

| `want` | Frame | Size | Polled every |
|---|---|---|---|
| 0 | **notifications** | 39 B idle · 84/161 B with 1/3 items | **5 s** |
| 1 | NOW | 164 B | 15 s |
| 2 | VELOCITY | 83 B | 120 s |
| 3 | PLAN | 71 B | 60 s |
| 4 | FRICTION | 141 B | 60 s |
| 5 | ANALYTICS | 127 B | 60 s |
| 6 | BURNDOWN | 147 B | 60 s |

Every answer also carries `next` (§4.2) and `dim` (§4.4); both are counted above.

The largest is 164 bytes against a write limit of roughly 182 (§2.3.1) — **18 bytes of headroom**,
which is the tightest margin in the design and worth watching. It cannot drift much further, because
**the screen bounds the frame**: 320×240 can only display so much, and the bridge already truncates
to exactly what fits (§3.1). A frame cannot outgrow the screen it feeds. If a future field does push
NOW over, §2.3.1 gives the one-argument fix.

Only two channels are ever active — notifications, and whichever screen is showing. That is
**12 polls per minute plus 0.5–4**, and roughly **950 bytes per minute**.

> The 673 bytes of all seven frames added together is a number with no meaning: nothing ever sends
> them as one object. An earlier draft treated that sum as a constraint and invented a round-robin
> scheduler to work around it. Both the constraint and the scheduler were imaginary.

#### 2.3.1 What the write limit actually is

macOS CoreBluetooth negotiates an ATT MTU of roughly 185, leaving about **182 bytes per
write-without-response**, well below the 512 an ESP32 accepts. Verified against `bleak` 3.0.2's
CoreBluetooth backend, which derives it as
`peripheral.maximumWriteValueLengthForType_(CBCharacteristicWriteWithoutResponse) + 3` — so the check
at D05 is one line: `client.mtu_size - 3`.

`bleak` does not split a write; an oversized buffer is the caller's problem. If the real limit ever
turns out smaller than a frame, the fix is one argument —
`write_gatt_char(..., response=True)` switches to `CBCharacteristicWriteWithResponse`, which uses long
writes and carries up to 512 bytes at the cost of a round trip. At these rates that cost is
irrelevant.

So this is a line in the test suite (§7), not an architectural risk.

### 2.4 The estimate runs 26–38% high — so BURNDOWN says "estimate", never "ideal"

Comparing the run's own `run.estimate` against what happened: **58 issues predicted, 42 delivered**,
with 10 of 15 versions mis-estimated.

This is why screen 6 labels its dashed reference line **estimate** and not *ideal*. "Ideal" implies
you are supposed to be on it, which would turn a systematically high estimate into a daily lie.
`/ship-phase` says the opposite in its own words — the estimate-versus-actual difference *is* the
measurement, "how well the roadmap predicts its own decomposition." The gap is the content of the
screen, not a defect in it.

---

## 3. Topology

```
  runs/<id>/events.jsonl                    the log; the only source of truth
        │  (tail + reduce)
        ▼
  dashboard/server.py  ── :8420 ──┬── ws://…/ws ──▶  browser     (24 KB frames)
   FastAPI, unchanged             │
                                  └── ws://…/ws ──▶  bridge/     (24 KB in)
                                                        │
                                          project(state, profile, screen)
                                          ALL computation happens here
                                                        │
                                              ┌─────────┴─────────┐
                                     ◀─Notify─┤                   ├─Notify─▶
                                     ─Write──▶│                   │◀──Write─
                                            Core2            StickC Plus2
                                        (draws; computes nothing)
```

**The devices are BLE peripherals; the laptop is the central.** `bleak` — the one BLE library that
works on macOS, because it wraps CoreBluetooth — is **central-only**, and macOS cannot practically act
as a BLE peripheral from Python. The ESP32 is a capable peripheral. One central holding two
peripherals is ordinary BLE.

**Not Bluetooth Classic / SPP.** PyBluez is unmaintained with no wheels for current Python, and
CoreBluetooth does not expose RFCOMM to userspace Python at all.

**The bridge is a client of the existing API, not a change to it.** It connects to
`ws://127.0.0.1:8420/ws` and receives the same frames the browser does:

- **The frontends cannot disagree.** All render one state object, projected differently. Two devices
  a metre apart showing different issue counts is the fastest way to destroy trust in both.
- **`dashboard/server.py` stays a pure reader** with no new imports. `bleak` never enters that
  process, so the dashboard still starts with no Bluetooth, no board, and no `bleak` installed.
- **A dead bridge is invisible** — [principle 2](ship-phase-tracking-vision.md) (*emission must never
  gate the pipeline*) extended one hop.
- **It is testable without a radio.** See §7.

### 3.1 The device renders JSON. That is all it does.

**No arithmetic beyond mapping a value to a pixel, no history, no logging, no state carried between
frames, and no partial redraws.** The bridge decides everything; the JSON carries the decision; the
device draws it. The whole firmware is four behaviours:

1. Ask for the current screen, and separately for notifications, at the intervals the bridge last
   gave (`next`).
2. A frame arrives → parse it → **repaint the whole screen** from it.
3. A notification arrives → buzz at the level given, switch screen if told to.
4. A button is pressed → ask for that screen immediately.

**What the bridge computes:** 30-minute velocity bucketing and quantisation to eight levels; the
issue-age median and its colour class; step-type medians and shares; progress percentages; time and
ETA formatting; retry rankings; finding counts by severity; string truncation to the target screen's
width; **which notifications to raise and how loud**; **which screen should be showing**; **how long
the device should wait before asking again**.

Earlier drafts leaked four things onto the device. Each is now gone:

| Was on the device | Now |
|---|---|
| Sparkline ring buffer | Bridge sends the finished series — full history in the first frame, nothing lost on power-cycle, and two devices cannot disagree |
| Alert state machine comparing successive frames | The notification channel delivers a finished instruction; arrival *is* the instruction |
| Auto-return-to-NOW timer | Bridge sends `g:1` once thirty seconds have passed |
| Dirty-region tracking | Impossible by construction — you cannot diff against a previous frame you never kept |

That last row is worth noticing: **full repaint is not a simplification we chose, it is what
statelessness leaves.** It costs roughly 110 ms against a 15,000 ms budget on NOW.

**Even a staleness timer is unnecessary.** It looked irreducible — if the bridge dies it cannot
announce its own death, so surely the device needs a clock to notice the silence. But the bridge *is*
its own BLE client; they are one process. Bridge dies, the connection drops, and the radio raises a
disconnect event. Out of range does the same. **The disconnect is delivered by hardware, not deduced
by logic.** The one remaining case — bridge alive, link up, upstream WebSocket dead — is something
the bridge can see itself and say in a notification.

The result is that `device/shared/` collapses to a JSON parser, and every claim on every screen
becomes testable in `pytest` without a board.

### 3.1.1 The one thing the device holds, and why it is not state

The device keeps the frame currently on the glass, because that is what rendering means — you cannot
draw a thing without the thing. It is one value, replaced on every write, gone on power-off. It is
not history, not a cache of the other screens, and never read except to repaint the pixels it already
describes.

This is why the **bridge**, not the device, decides which screen is displayed: it either told the
device (`g`) or was told (`want`). A button press is not resolved locally — the device asks, and the
bridge answers.

### 3.2 `project(state, profile, screen)` — one function, per-device output

Because the bridge formats for a specific screen width, the two boards do not receive identical
bytes. An earlier draft claimed they did and offered that as proof the contract was right; that claim
is retired. The function stays pure and golden-testable, it simply takes a device profile and a
screen number. The StickC benefits: fewer screens and shorter strings mean smaller frames.

### Layout

```
codegen/
├── bridge/          BLE central: subscribe → answer polls                (Python, bleak)
│   ├── project.py     (state, profile, screen) → JSON. Pure. All computation lives here.
│   ├── notify.py      the notification queue and its buzz levels
│   ├── transport.py   BleakTransport (one per device) + FakeTransport
│   ├── devices.py     the roster: name, address, profile
│   └── main.py        the loop; --fake-device runs it with no hardware
└── device/          firmware for both boards                              (C++, PlatformIO)
    ├── prototype.html   the six screens, rendered from the real frames — a design
    │                    reference, never served and never flashed
    ├── platformio.ini   two [env:] targets over one shared lib
    ├── shared/          the JSON frame parser. That is the whole shared library —
    │                    §3.1 left nothing else to share. Host-testable.
    ├── core2/           320×240 renderers · vibration + speaker
    └── stickc/          240×135 renderers · buzzer + LED · sleep policy
```

This obeys [principle 6](ship-phase-tracking-vision.md): everything lives under `codegen/`, and
`rm -rf codegen/` still leaves a working repo. The device work survives `/reset-generated` because it
is not generated.

---

## 4. The link

**GATT service** — one service, three characteristics:

| Characteristic | Direction | Properties | Carries |
|---|---|---|---|
| `input` | device → laptop | Notify | `{"want":0}` or `{"want":4}` — 10 B |
| `frame` | laptop → device | Write Without Response | the answer, 39–164 B |
| `info` | device → laptop | Read | board type, firmware version, schema `v` understood |

Write Without Response is right here because a lost answer costs one interval — the device asks again
five to sixty seconds later. There is nothing to acknowledge, because there is no push to lose.

`info` lets a bridge speaking schema 2 to firmware that knows only 1 say so on screen rather than
rendering nonsense.

### 4.1 Two independent poll loops

**Notifications and screen data are separate channels with separate latencies**, and separating them
is what makes both rates honest:

- **Notifications, `want:0`, every 5 s.** Not because anything changes that fast — 57 notifications
  across 4.1 hours is one per four minutes — but because this is the channel that can buzz, and how
  late a buzz may be is the only genuine latency requirement in the system.
- **The active screen, `want:N`, every 15–120 s** (§2.3). Driven purely by how fast that screen's own
  data changes, now that it is no longer dragged along by the notification rate.

Combined, that is 880 B/min instead of 1,848 B/min for a single 5-second loop carrying everything —
**52% less traffic** for four more round trips. The idle notification response is 29 bytes, and idle
is the overwhelmingly common case.

### 4.2 `next` — the bridge sets the pace

Every answer carries `next`, the seconds to wait before asking again. The device stores no
configuration and makes no decision; it obeys the last number it was given.

| Run state | `next` for screens | Why |
|---|---|---|
| active | 15–120 s | as measured in §2.3 |
| **finished** | 60 s | the state is frozen — nothing will ever change again |
| no runs | 60 s | nothing to look at |

The finished case is the one that earns the field: after `run.end` a device may sit on a desk for
days, and polling every fifteen seconds forever is pure waste. The bridge simply says *ask again in a
minute*.

### 4.3 Liveness needs no timer

The reasoning in [dashboard/server.py:37-48](dashboard/server.py#L37-L48) still holds — during the
v01–v03 run 23 gaps ran over a minute and the longest was 26, and a frozen screen beside a "live"
indicator is not a slow device but a lying one. The device answers it without logic: when the link
goes down the radio says so (§3.1). Closed lid, killed bridge, out of range — all three become the
same disconnect event, and the device draws the disconnected screen.

### 4.4 Dimming and wake

Brightness steps down on a ladder **derived from the NOW poll interval** (§2.3), not from hand-picked
constants — so if that interval is ever retuned, the ladder follows instead of quietly becoming
wrong.

| Idle for | **Core2** | **StickC Plus2** |
|---|---|---|
| — | 100% | 100% |
| 2× interval · 30 s | **50%** | **off** |
| 3× interval · 45 s | **20%** | — |

**The Core2 never goes dark**, and that is the whole reason §1 can still claim it is always visible.
At 20% it is dim enough to stop being a light source on a desk at night and bright enough to read
across the room. **The StickC does go dark**, because it is a pager: its job between notifications is
to be a lump in a pocket, and 200 mAh does not fund anything else. The split falls exactly along the
roles in §1.1 — one board is read, the other is felt.

**The bridge owns the ladder, as it owns everything else.** Each answer carries `dim` (100, 50, 20 or
0) and the device just sets the backlight. A 15 s poll is fine-grained enough to land on both steps,
and the device runs no timer — §3.1 still holds with nothing carved out.

**Every way of waking it is the same message.** A button press, or a tap on the Core2's touch screen,
sends `{"want":N}` — a tap simply keeps the current N. The bridge reads any incoming `want` as *the
user is present*, restarts the ladder, and answers `dim:100`. There is no tap protocol, no wake
protocol, and nothing on the device to remember.

**Any notification wakes the screen too, including a silent one.** This makes `b` cleanly one thing:
it controls sound and vibration only, never the backlight. A `release.tagged` at `b:0` still lights
the panel — quietly, which is the point. At one notification per four minutes (§5.1) the Core2 spends
a fair share of a run at full brightness without anyone touching it, which is the behaviour you want:
bright exactly when something just happened.

**Polling continues while dimmed.** It must — that is how the device learns a notification arrived.
On the Core2 that costs nothing on USB power, and on the StickC `next` stretches to 60 s the moment a
run finishes (§4.2).

---

## 5. The screens

Six on the Core2, all rendered in [device/prototype.html](device/prototype.html) from the exact frames
in §2.3. **Dark only** — the device sits in a lit room, and the palette is copied verbatim from the
dark block of `dashboard/static/styles.css`, whose dark series colours are chosen for a dark surface
rather than flipped from the light ones. One palette is one palette to keep validated, and the panel
reads as the same product as the browser.

| # | Screen | Answers |
|---|---|---|
| 1 | **NOW** | where is it, and is this issue stuck? |
| 2 | **VELOCITY** | is it still moving? |
| 3 | **PLAN** | where are we in the roadmap? |
| 4 | **FRICTION** | what needed a human? |
| 5 | **ANALYTICS** | where did the time go? |
| 6 | **BURNDOWN** | will it finish? |

**NOW** leads with the version and issue — the most-asked question during a run — with progress as a
ring rather than a bar, because a ring reads as a proportion across a room. Beside the step is the
**issue-age pill**, coloured against the median for that step type. It is the most decision-changing
element on any screen: issue duration has a **13× spread** (median 1.7 min, max 23.2 min), so "running
15 minutes" is a signal that exists nowhere else today. The elapsed clock shows **minutes, not
seconds**, because at a 15-second poll a seconds display would visibly jump.

**VELOCITY** shows issues closed per 30 min. The measured run reads `15 · 7 · 2 · 2 · 6 · 4 · 6` — a
real stall between 1.5 and 2 hours that no other view surfaces.

**PLAN** groups the fifteen versions by phase rather than in one flat strip, mirroring the roadmap and
staying legible; the running version carries a halo so it reads without hunting.

**FRICTION** is deliberately not called *failures*. The measured run had **zero** `issue.failed`
events — a screen named for failure would have been empty for 4.1 hours, which is the worst kind of
screen: you stop looking, and then it has something to say. The real content is **10 retries out of 42
issues** and **9 findings** (6 MEDIUM, 3 LOW).

**ANALYTICS** shows where the time went **by step type, with medians rather than sums**. That is what
makes it survivable on damaged data (§9): a single unclosed 155-minute node distorts a sum and cannot
distort a median. It also surfaced the run's largest anomaly — `upload-issues` has a median of **0:49**
and a maximum of **22:07**, a **27× spread** invisible in any per-version view. The `sample 42%` badge
is mandatory, not decorative: the table is built from closed spans covering 103 of 245 minutes, and
the screen must say so.

**BURNDOWN** plots remaining issues, built from `issue.closed` rather than `state.burndown`, which is
non-monotonic and unusable (§9). The dashed line is the **estimate** (§2.4), and the projection is a
**cone to the ETA range, never a single line**, matching the rule the ETA already follows. The first
four points sit flat at 46: the opening ~50 minutes close no issues, because `generate` and `upload`
produce no closures. That shelf is real, and the screen shows it rather than smoothing it away.

### 5.1 Notifications — alerts and events in one channel

The `want:0` channel carries **everything that happened at a point in time**, as against the screens,
which carry current state. Alerts and events are not two types: they are one list, and the only thing
separating them is `b`, how loud it is.

```json
{"v":1,"s":0,"next":5,"n":[
  {"k":"release","t":"v05.03 tagged","b":0},
  {"k":"retry","t":"RF-086 x4","b":2,"g":4}
]}
```

`k` kind, for the icon and colour · `t` the line of text · `b` volume 0–3 · `g` screen to switch to.

**`t` is composed from identifiers, never copied from the log.** `"RF-086 x4"` and
`"v05.03 tagged"` are built by the bridge out of an issue id, a version and a count — not an issue
title, a commit message or a finding's description. This is the same rule the screens follow, and it
is what keeps [architecture.md §8](architecture.md) (redaction) out of scope entirely: a frame that
cannot contain log text cannot leak a secret, so nothing needs redacting on the way to the device.

| | Notification | In the run | `b` |
|---|---|---|---|
| event | `run.end` — work finished | 1 | 1 · chime |
| event | `phase.end` — phase finished | 5 | 1 · chime |
| event | `release.tagged` | 15 | 0 · silent |
| event | `version.end` | 15 | 0 · silent |
| alert | `issue.retry` | 10 | 2 · short buzz |
| alert | `finding.raised` | 10 | 2 · short buzz |
| alert | `issue.failed` | 0 | 3 · long buzz |
| alert | `harden.finding.held` | 0 | 3 · long buzz |
| alert | `gate.blocked` | 1 | 3 · long buzz |

**57 notifications across 4.1 hours — one per four minutes**, against a five-second poll. The queue is
almost always empty; three items is the most that fits one write and a burst that large is close to
impossible. The bridge holds the queue, so overflow simply waits for the next answer.

**Zero `issue.failed` in this run is not a reason to drop it.** Nothing failed here, but those are the
notifications that matter most when they do happen. The same argument as the FRICTION screen: empty
does not mean unnecessary.

**A lost buzz does not lose information.** The bridge drops a notification from the queue once it has
been answered, so a lost write costs one vibration. That is acceptable because **the buzz is the
notification and the screen is the record** — FRICTION still shows the retry and the finding whenever
you look. The alternative, acknowledgement plus dedupe by id, would put comparison-against-previous
back on the device, which §3.1 exists to prevent.

### 5.2 Graphics — real vector drawing, not pseudographics

The frames carry **numbers and ASCII flags**, never glyphs: `"sp":"7511232"`,
`"vs":"##############>"`, `"bd":"4646464633…"`. The firmware turns them into geometry with LovyanGFX —
`drawSmoothArc` for the ring, `drawSmoothLine` for curves, `fillSmoothCircle` for version dots,
`fillSmoothRoundRect` for bars and chips, drawn into an off-screen `LGFX_Sprite` so nothing flickers.

Two consequences:

- **No font work.** The only glyphs ever rendered are `[A-Za-z0-9 .:/%-]`, which the stock Latin font
  covers. No Cyrillic range, no custom glyph set, and no box-of-question-marks that appears only on
  hardware. A one-line test asserts every frame is pure ASCII, so a stray `·` or `–` fails CI instead
  of the panel.
- **Quality is effectively free.** A full-screen RGB565 sprite is **150 KB — 1.8%** of the Core2's
  8 MB PSRAM, and a complete anti-aliased repaint costs roughly **110 ms** against a 15,000 ms budget
  on NOW. The usual reason embedded UIs look crude — no time in the frame — does not apply at these
  rates.

---

## 6. Input — and how six screens stay ambient

Buttons choose the screen; a tap on the Core2's touch panel restores full brightness without changing
it. That is the whole input surface, and both are the *same message* — per §2.2 and §4.4 a press is
not handled locally: it changes which screen the device asks for, a tap keeps the current one, and
either way the request goes out immediately. Switching never waits for an interval, and there is no
separate tap or wake protocol to maintain.

Six screens would contradict §1's "ambient annunciator, not a small dashboard" **if you had to hunt
through them**. Two rules remove the hunting, and both live on the bridge:

- **Auto-navigation on alert.** A retry arrives with `g:4`, and the device is on FRICTION. You never
  search for the relevant screen; it arrives.
- **Auto-return to NOW.** Thirty seconds after the last press the bridge sends `g:1`. The resting
  state is always the same screen, and the timer deciding it is in Python, not C++.

**Neither device ever acts on the pipeline.** No approving a gate, no retrying a step, no answering an
`AskUserQuestion`. A physical "yes" button is tempting, and the StickC in a pocket makes it more so,
and it is ruled out:

- It would make an **ambient display into a control surface** — a thing designed to be glanced at,
  forgotten, and put things on top of. You must not be able to sit on a control surface.
- An accidental press would **change what gets built**, with no record of why in the resulting commit.
- It breaks [principle 5](ship-phase-tracking-vision.md) — *the dashboard is a renderer; it holds no
  authority.* The boards are further clients of the same state, exactly as `web/` is a client of the
  game server. The value of that rule is that any renderer can be unplugged at any time with no
  consequence. One button would end it.

---

## 7. Testing — nearly all of it without hardware

Matching [architecture.md §10](architecture.md). Because §3.1 moved every computation to the bridge,
the untested surface is smaller with two devices than it was with one.

- **`project(state, profile, screen)` is a pure function.** Golden-frame tests over real reduced
  states from `runs/`, per profile, per screen. Every statistic on every screen is verified here, in
  `pytest`, with no radio and no board.
- **Frame guards.** `len(frame) <= 182` and `json.dumps(frame).isascii()`, for every screen and for a
  notification response holding three items. Both are one-liners; both catch a class of bug that would
  otherwise only appear on the panel.
- **Notification logic is a bridge test.** Which events raise a notification, at what volume, the
  queue draining in order, `g` on alert, `g:1` after thirty seconds, `next` changing when the run ends
  — all `pytest`, none of it needing a buzzer.
- **`FakeTransport` answers polls.** Poll cadence, `want` routing, reconnect, dropped writes, schema
  mismatch — in milliseconds. **Two fakes cover both devices**, including the case that matters: one
  device dropping must not stall the other.
- **`bridge/main.py --fake-device`** runs the real loop end to end against a real dashboard with no
  hardware, which is also how it runs in CI.
- **Replay drives it.** [tests/replay.py](tests/replay.py) already pushes a recorded log through the
  system, so a four-hour run replays in seconds against whichever boards are on the desk.
- **`device/shared/` is compiled and tested on the host**, and after §3.1 it is only the JSON parser.
  Malformed, truncated and future-schema frames each get a test, because all three will happen.

What genuinely needs hardware: two displays, two buzzers, the LED, and the BLE peripheral. That is
checked by eye — exactly as the dashboard prototype's four visual bugs were
([architecture.md:528](architecture.md#L528)). Everything that could be *wrong rather than ugly* is
verified in Python.

---

## 8. Implementation plan

Ordered so everything hardware-independent is finished, tested and reviewable **before a board is
plugged in**.

| # | Stage | Hardware? | Delivers |
|---|---|---|---|
| **D01** | Frame contract + `project()` | no | Seven frame schemas, the pure projection with all statistics, golden tests, the size and ASCII guards |
| **D02** | Notifications + `next` | no | The queue, the volume catalogue (§5.1), `g` navigation, the pacing policy — all in `pytest` |
| **D03** | Bridge + `FakeTransport` | no | Poll answering, `want` routing, reconnect-with-backoff, `--fake-device` |
| **D04** | `device/shared/` | no | The JSON parser, host-compiled and tested |
| **D05** | Core2 firmware + BLE | Core2 | GATT service, **MTU verification (§2.3.1)**, the two poll loops, six screens against `prototype.html` |
| **D06** | Core2 output | Core2 | Vibration and chime per volume level, navigation on `g`, the disconnected screen |
| **D07** | StickC Plus2 port | StickC | Second `[env:]`, 240×135 layouts, buzzer + LED, sleep-and-wake policy |
| **D08** | Both at once | both | Two simultaneous peripherals, independent poll loops, per-device reconnect |

D01–D04 are ordinary code in this repo's existing test suite and need no purchase. After §3.1 they
carry **the majority of the system's logic**, including every graph and every notification decision.
D05 is where a board is first required, and its first task is to verify the 182-byte assumption —
the one number here that could still be wrong.

**The StickC comes after a working Core2 (D07, not D05).** Porting a proven renderer to a second
screen is a known quantity; debugging BLE, the poll protocol and a new layout at once is not.

**Firmware stack: PlatformIO + M5Unified + NimBLE-Arduino.**

- **M5Unified**, not the per-board legacy libraries — one API across both, and it absorbs two real
  footguns: Core2 v1.0 and v1.1 use **different power-management chips** (AXP192 vs AXP2101), and the
  StickC Plus2 requires **holding GPIO4 high to stay powered on**.
- **LovyanGFX** (under M5Unified) supplies the anti-aliased primitives and sprites §5.2 relies on.
- **NimBLE-Arduino**, not Bluedroid: roughly half the RAM, which matters more on the StickC.
- One `platformio.ini`, two `[env:]` targets, one shared `lib/` — what keeps `shared/` genuinely
  shared rather than copy-pasted and drifting.

**Power.** Neither battery covers a 4.1-hour run with the screen lit — 390 mAh and 200 mAh. The
conclusion differs per board and each is a decision, not a caveat: the **Core2 is USB-C powered on the
desk** with its screen lit; the **StickC runs on battery with its screen asleep**, waking for
notifications and button A. Polling rather than receiving pushes helps here — the radio is idle
between polls, and `next` stretches to 60 s the moment a run finishes.

---

## 9. Known-bad data this design works around

Building the screens surfaced defects in the tracking system's own instrumentation. They are recorded
here because §5 is shaped around them, and because they are worth fixing independently — the browser
dashboard consumes the same values.

1. ~~**Unclosed step nodes accrue elapsed against wall-clock.**~~ **Fixed** ([#113](https://github.com/ichMaster/agent-arena-sandbox/issues/113)).
   8 of 44 step nodes had `status=running, end=None`, and one `execute-issues` node reported
   **620,225 s (172 h)** inside a 4.1-hour run, with step totals summing to 174 h. `reduce()` now caps
   an unclosed node at its parent's end rather than at `now`: on the same log the totals fall to
   **2.4 h** and the longest step to 2,426 s. A finished run now reduces identically whenever you look
   at it, which is the property whose absence was the bug.
2. **Step events are missing for 7 of 15 versions.** Only 8 versions emitted any `step.end`. This is
   emission discipline in `/ship-phase` degrading over a long run, not a reducer fault — and
   discipline is the wrong fix. The reducer should derive step spans from `scope.step`, which every
   event already carries, leaving explicit `step.start`/`step.end` as a refinement.
3. **`state.burndown` is non-monotonic.** `known_points` oscillates `0 → 7 → 0 → 5 → 0`; it tracks
   decomposition, not completion, and cannot drive a progress curve. Screen 6 uses `issue.closed`
   instead.
4. **`state.current` is degenerate.** In the measured run it reduces to
   `"execute-issues · execute-issues · execute-issues"` — the step name repeated three times, with no
   version and no issue. It appears to be assembled as a path through the tree but picks the wrong
   nodes. The `version · issue · step` label NOW needs is therefore not in the state at all.
   The bridge builds it instead, from the deepest tree node whose status is *running* (D01). Note it
   can only be verified against a **live** run — a finished run has no running node, so the bug is
   invisible there.

**How §5 survives them:** ANALYTICS uses medians over closed spans and states its coverage
(`sample 42%`); BURNDOWN reads `issue.closed` directly. Neither waited on a fix, and neither needs
revisiting now that item 1 has landed.

**`cov` measures item 2, not item 1.** Fixing the reducer did not move it — coverage is closed-span
minutes over run minutes, and capping an unclosed node does not close it. `cov` reaches 100 only when
the skills stop dropping `step.end`, which is why the badge is the honest readout for *that* gap
specifically.

**These cannot be validated against this log** — the data is already lost. Verification needs a fresh
`/ship-phase` run, which the repo's generate → observe → reset → regenerate cycle produces anyway.

---

## 10. Decisions taken

Nothing here is open any more. A design doc gets read later by someone asking "did they think about
X?", so the answers stay, with their reasoning, instead of being deleted once made.

### 10.1 No pairing or bonding — an accepted risk

**Decided:** the link is unprotected. Any central in range can connect and answer polls.

**The risk, stated plainly:** a forged frame reading "all green" would be rendered faithfully, and
believed. This is the one way the screen can lie that nothing else in the design prevents — worth
naming precisely because the rest of the document works so hard to stop exactly that.

**Why it is accepted:** the attack needs someone inside BLE range who knows the service UUID and
actively wants to mislead you about a build monitor on your own desk. That does not justify pairing
friction on a board that gets re-flashed often.

**The practical consequence is occupancy, not security.** A BLE peripheral serves one central at a
time, so whichever machine connects first has it. The bridge reconnects when it frees up; nothing is
lost and nothing is corrupted.

Revisit if a device ever moves from a trusted space to a shared one.

### 10.2 The brightness ladder — derived rather than chosen

**Decided:** steps at 2× and 3× the NOW poll interval (§2.3), which is 15 s — so 30 s and 45 s. The
Core2 goes 100% → 50% → 20% and never dark; the StickC goes 100% → off at the first step. Full
detail in §4.4.

**Why a rule beats a constant.** A lit screen always spans at least one refresh, so you are never
reading a snapshot frozen at the instant the buzz fired. And if the NOW interval is ever retuned, the
ladder follows automatically instead of quietly becoming wrong — which is what would eventually
happen to a hand-picked "8–10 seconds".

**Why the two boards differ.** It is the §1.1 split, not an inconsistency: the Core2 is *read*, so it
stays readable at 20%; the StickC is *felt*, so it sleeps. A 200 mAh battery does not fund a lit
screen either way.

It is all a **bridge** parameter — `dim` rides in every answer (§4.4), so tuning any of it never
means reflashing.

### 10.3 Out of v1 on purpose

Recorded so they are not re-litigated, and so the reasoning survives:

- **Per-device run selection.** Both devices follow whichever run is active. Showing different runs
  per device would mean the bridge polling `/api/state?run_id=…` separately and holding a distinct
  projection per device. The *protocol* already has room — `want` could carry a run id beside the
  screen number — so this is deferred bridge work, not a design gap.
- **A third device.** Nothing prevents it, and polling makes it cheaper than push would have:
  devices share no schedule, they just ask when ready. A third board costs one row in `devices.py`
  plus a profile. Nobody has asked for one.
- **Any control over the pipeline.** Permanently out, for the three reasons in §6 — not deferred.
