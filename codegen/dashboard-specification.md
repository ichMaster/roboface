# Codegen dashboard — UI specification

**Status:** specification. The reference implementation is
[dashboard/prototype.html](dashboard/prototype.html), which is complete and renders in both themes;
what remains is splitting it into files and binding it to live data (TRK-019/TRK-020).

**Companions:** [ship-phase-tracking-vision.md](ship-phase-tracking-vision.md) decides *which* panels
exist and *why* each takes the form it does — this document does not re-argue that.
[architecture.md](architecture.md) defines the data reaching this UI. This file covers **how the UI is
built**: tokens, components, DOM, rendering, interaction, accessibility.

> Not to be confused with [`specification/ARCHITECTURE.md`](../specification/ARCHITECTURE.md), the
> architecture of the *generated product*. Same house style, zero shared code.

---

## 1. Principles & constraints

1. **No build step.** Hand-authored HTML/CSS/JS served as-is, matching the generated app's approach.
   No bundler, no transpiler, no framework, no chart library.
2. **No external requests, ever.** No CDN, no web font, no analytics. A strict reading of vision §3
   principle 6: the dashboard works offline, on a laptop, with the application tree deleted.
   Third-party assets, if any are ever needed, are vendored into `static/vendor/` — **never `lib/` or
   `dist/`**, which the repo's inherited `.gitignore` silently swallows (architecture §4.1).
3. **The UI is a renderer with no authority.** It holds no state the log doesn't, computes no metric
   the reducer doesn't, and can be deleted mid-run without affecting anything.
4. **Charts are hand-built SVG.** A charting library would cost an external dependency and take away
   control of the exact mark specs in §2.3 — which are the difference between this looking considered
   and looking generated.
5. **Every value is reachable without hovering.** Tooltips enhance; the table view is the guarantee.

---

## 2. Visual design system

### 2.1 Colour tokens

The palette is **validated, not chosen by eye** — five categorical slots passing every check in both
modes. Do not edit a hex without re-running the validator (TRK-021 makes that a test).

Declared as CSS custom properties on `.viz-root`, with dark values under **both** a
`prefers-color-scheme` block and a `:root[data-theme="dark"]` block so the in-page toggle wins in
either direction:

| Role | Light | Dark | Used for |
|---|---|---|---|
| `--surface-1` | `#fcfcfb` | `#1a1a19` | card surface, mark gaps and rings |
| `--plane` | `#f9f9f7` | `#0d0d0d` | page behind the cards |
| `--text-primary` | `#0b0b0b` | `#ffffff` | values, direct labels |
| `--text-secondary` | `#52514e` | `#c3c2b7` | legends, axis titles |
| `--text-muted` | `#898781` | `#898781` | axis ticks, captions |
| `--grid` | `#e1e0d9` | `#2c2c2a` | gridlines (hairline, **solid**) |
| `--axis` | `#c3c2b7` | `#383835` | baselines |
| `--series-1…5` | `#2a78d6 #eb6834 #1baf7a #eda100 #e87ba4` | `#3987e5 #d95926 #199e70 #c98500 #d55181` | categorical identity |
| `--seq-200…600` | blue ramp | blue ramp | sequential magnitude (heatmap) |
| `--deemph` | `#d6d5cf` | `#3a3a37` | the "rest" in emphasis charts |
| `--good/warning/serious/critical` | `#0ca30c #fab219 #ec835a #d03b3b` | same | **status only**, never a series |

Three rules that are not negotiable:

- **Text never wears a series colour.** Marks carry identity; labels use text tokens. A colour dot or
  line-key sits *beside* the text.
- **Status colours are reserved.** They never stand in for "series 6", and always ship with an icon and
  a word — never colour alone.
- **Light-mode relief.** Aqua, yellow and magenta sit below 3:1 on the light surface. That is legal
  *with relief*, which here means **direct labels on every bar end plus the table view** — both already
  implemented. Removing either breaks the palette's validity in light mode.

### 2.2 Type & spacing

System sans throughout (`system-ui, -apple-system, "Segoe UI", Roboto, sans-serif`) — no display face,
including on the hero figure. Base 14px/1.45.

| Token | Value |
|---|---|
| Hero figure | 48px, 600, `letter-spacing:-.02em`, **proportional figures** |
| Stat value | 25px, 600 |
| Card title | 12.5px, 600 · card note 11.5px, muted |
| Axis tick | 10.5px muted, `tabular-nums` |
| Direct label | 11px, 600, `--text-primary` |
| Card radius 12px · card padding 14px 16px · grid gap 14px |

`tabular-nums` **only** where digits align vertically — axis ticks, table cells, tree durations. Never
on the hero or stat values; equal-width digits make `121` look loose at display size.

### 2.3 Mark specs

Fixed across every chart. These are what the prototype implements.

| Mark | Spec |
|---|---|
| Bar / column | ≤ **24px** thick; **4px rounded data-end, square at the baseline** |
| Line | **2px**, round join and cap |
| End marker | **r = 4.5** (≥ 8px diameter), filled series colour, **2px `--surface-1` ring** |
| Area fill | series hue at **~10%** opacity |
| Uncertainty band | series hue at **~16%** opacity |
| Gridline / axis | 1px, **solid**, one step off the surface |
| Separation | a **2px gap in the surface colour** between touching marks — never a stroke around them |

**Dashes are reserved for projections.** The burn-down's ideal line is dashed *because it is a
projection*; that is the only dashed element on the page. Dashed gridlines are forbidden — they read as
a threshold that isn't there.

---

## 3. Layout

- Page: `.viz-root` → `.wrap` at `max-width: 1240px`, 20px padding.
- Content: a **12-column CSS grid**, 14px gap. Cards span via `.c12 / .c8 / .c6 / .c4`.
- Breakpoints: **900px** collapses every card to full width; **1000px** and **620px** step the KPI grid
  from 4 → 2 columns.
- Card anatomy: `.card > .cardhead (h2 + .note + .spacer + table toggle) > chart container > .legend >
  table container`.

Panel order top to bottom is fixed — it is a reading order, not a preference: what is happening now
(header, KPIs), then the tree, then projections (burn-down, velocity), then analysis (time, failures,
suite, findings).

**Every card must size to include its axis band.** A fixed height that clips the x-axis produces a tiny
nested scrollbar; either size the container to plot + axis, or let it grow with content.

---

## 4. Components

### 4.1 Filter row

One row, above everything it scopes (`run`, `phase`, `status`). Never inside a card, never per-chart.
Ordinary `<select>` elements styled to the card chrome. Changing any filter re-renders **every** panel
against the same slice, so the numbers always agree.

### 4.2 Run header

Hero figure (elapsed, live-ticking) + current node as text + status chip + **ETA as a range**. Exactly
one hero per view. ETA renders blank — not "0", not "—" — until `state.eta` is non-null, and always
shows its basis line beneath (`from N issues sampled · M versions not yet decomposed`).

### 4.3 Stat tile

`label` / `value` / `delta`. The label **names the unit and the subject** ("Mean time per issue", not
"Velocity"); the delta **names its comparison and shows that value**, and states direction **in words**
("12s faster than v01.02 (5:42)"). Colour agrees with the word; it never carries the meaning alone.
Percentages below ~2% are suppressed in favour of the absolute difference.

### 4.4 Status indicator

`.status` = a 9px dot + a word. Five states: `running` (series-1, pulsing), `ok` (good), `fail`
(critical), `held` (serious), `skipped` (muted). The word is always rendered; the dot never carries
state by itself. Under `prefers-reduced-motion` the pulse is replaced by a static dot.

### 4.5 Run tree

An indented list, not a chart — five nested levels of state exceed what colour can encode. Rows are
`.tnode` with `.d1/.d2/.d3` indent classes, a status chip, a flexible name that ellipsises, an optional
tag, and a right-aligned `tabular-nums` duration. The active branch is expanded and its row is bold;
completed branches collapse to their version row.

### 4.6 Legend

Always present for **two or more** series; **absent for one** (the title names it). Key shape mirrors
the mark: an 11px rounded square for bars/areas, a 14×2px bar for lines. Legend text uses
`--text-secondary`, never the series colour.

### 4.7 Tooltip

A single `#tip` element outside all panels, `position: fixed`, so it survives panel re-renders.

- **Built with `textContent` only.** Issue titles, findings and branch names originate in model output
  and tool results — untrusted. Never `innerHTML` with interpolated data.
- **Value leads, label follows** — the reader already knows the series and wants the number.
- Rows key their series with a short **line stroke**, not a filled box.
- Positioned above-right of the pointer, clamped to the viewport.
- Shown on `pointermove` **and on `focus`**, hidden on `pointerleave`/`blur`.

### 4.8 Hit targets

Every interactive mark carries a transparent `.hit` rect wider than the mark itself — the full band for
line charts, the bar plus its 2px gap plus padding for bars. Never require landing on painted pixels.

### 4.9 Table view

Every chart has one. A `.tv` toggle button in the card head with `aria-pressed`; the table appears
**below the chart, without replacing it**. Right-aligned `tabular-nums` numerics, left-aligned first
column. This is the WCAG-clean twin and the relief for the light-mode contrast finding — it is not
optional on any panel.

---

## 5. Panels

Nine panels; forms and rationale are vision §6.2. Implementation notes only here.

| # | Panel | Container | Construction |
|---|---|---|---|
| 1 | Run header | `.hero` | DOM, no SVG |
| 2 | KPI row | `#kpis` | DOM, 8 tiles |
| 3 | Live tree | `#tree` | DOM, recursive from `state.tree` |
| 4 | Burn-down | `#c-burn` | SVG: band path, dashed ideal, 2px line, end marker |
| 5 | Velocity | `#c-vel` | SVG: bars + per-issue spread dots |
| 6 | Where time went | `#c-time` | SVG: horizontal stacked bars, 2px surface gaps |
| 7 | Failure surface | `#c-fail` | SVG: emphasis bars, rotated tick labels |
| 8 | Suite trajectory | `#c-suite` | SVG: area at 10% + 2px line + end marker |
| 9 | Quality flow | `#c-q` | SVG: horizontal stacked bars |

Each panel is a **pure function of state**: `render(state) → html string`, assigned to its container's
`innerHTML`, then hit-targets bound. No panel reads the DOM to decide what to draw.

**SVG geometry.** `viewBox="0 0 W H"` with `width:100%; height:auto; overflow:visible`, so panels scale
without a layout pass. Scales are trivial closures (`x = v => L + v/maxX * (W-L-R)`), not a library.
Axis padding constants live at the top of each panel function.

**Stacked segments** are built as explicit paths so only the first and last segment round their outer
corners, and each segment's width is `scale(value) - 2` — the 2px gap is subtracted from the mark, not
drawn over it.

**Two panels have data-honesty rules that are part of the spec, not styling:**

- **Panel 9 excludes versions whose review step has not run.** Zero findings and *not yet reviewed* are
  different claims; drawing the latter as an empty bar asserts "clean". The excluded versions are named
  in a note beneath the chart.
- **Panel 4 plots the projection, not known-work.** Known-remaining touches zero at every version
  boundary and reads as "finished". The known figure lives in the tooltip and table view.

---

## 6. Data binding

### 6.1 Transport

`WS /ws` on port **8420**. On connect the server sends one `snapshot` frame containing the full reduced
state; thereafter one `delta` frame per appended event carrying the updated state (the state is small —
diffing it would cost more than sending it).

```
→ {"kind":"snapshot","state":{…}}
→ {"kind":"delta","state":{…},"event":{…}}
```

`GET /api/state` returns the same object for a no-JS/no-WS read.

### 6.2 Reconnect

Exponential backoff from 500 ms to 10 s. On reconnect, request a fresh snapshot rather than resuming —
the state is authoritative and cheap. While disconnected, the header shows a `stale` status chip and
**the last render is retained**; panels are never blanked.

### 6.3 Update discipline

- **Debounce re-renders to ≤ 5 Hz.** Logs arrive in bursts; re-rendering per event would thrash.
- **Hold the previous render.** During an update, the panel keeps its content at reduced opacity. No
  skeletons, no blanking, no layout jump — with events every few seconds a flashing page is unusable.
- **Never re-render a panel containing the focused element.** Defer until blur; otherwise keyboard
  users lose their place mid-inspection.
- Colour follows the **entity**, never its position: filtering to three versions must not repaint the
  survivors. Series index is derived from a stable key (step name, outcome name), never from array
  order.

---

## 7. Client state

Minimal, transient, non-authoritative — the same posture the generated app's UI takes.

```js
{ state,            // last reduced state from the server; the only data source
  filters,          // run / phase / status — UI-local
  theme,            // null (follow OS) | 'light' | 'dark'
  openTables }      // per-panel table-view toggles
```

Nothing else is cached, nothing is computed that the reducer already computes, and a reload with the
same `state` produces an identical page.

---

## 8. Accessibility

- **Identity is never colour-alone.** Legends for ≥2 series, direct labels, status words, table views.
- **Keyboard**: every `.hit` is `tabindex="0"` and shows on focus exactly what hover shows. Table
  toggles are real buttons with `aria-pressed`. Focus rings are never removed.
- **Contrast**: text meets 4.5:1 in both themes. The three sub-3:1 light-mode series colours carry the
  §2.1 relief.
- **Motion**: `prefers-reduced-motion` disables the status pulse and any transition beyond opacity.
- **Live region**: the tooltip is `role="status" aria-live="polite"`; the run status chip announces
  transitions (running → ok/fail) and nothing else, so the page is not chatty.
- **Zoom**: usable at 200% — the grid collapses at 900px, which zoom reaches.

---

## 9. Performance budget

| Constraint | Budget |
|---|---|
| Full re-render, all nine panels | < 50 ms |
| Frame → paint | < 500 ms |
| Idle CPU with a live run | ~0 (event-driven; only the elapsed clock ticks, at 1 Hz) |
| Page weight | < 100 KB total, zero external requests |

A run of 10 000 events must not degrade the page — the client renders `state`, never the log, so cost
is independent of run length.

---

## 10. Out of scope

Cost and token accounting (decided out of scope — not observable from inside a run) ·
authentication (binds to localhost) · mobile-first layout (usable at 900px, not designed below it) ·
editing anything from the UI (it is read-only by construction) · historical charting beyond the
cross-run views in TRK-022 · export, print stylesheets, and i18n.

---

## 11. Acceptance criteria

Maps to TRK-020 in [implementation-plan.md](implementation-plan.md).

- [ ] Nine panels render from a real reduced run, no mock data remaining.
- [ ] Split into `static/index.html` + `app.js` + `styles.css`; no build step; no external request in a
      devtools network trace.
- [ ] Snapshot-then-delta binding works; a client connecting mid-run converges to the same state as one
      connected from the start.
- [ ] Updates hold the previous render — no flash, no layout shift — and never steal focus.
- [ ] Every chart has a working table view; every value is reachable without hovering.
- [ ] Keyboard reaches every interactive mark and shows what hover shows.
- [ ] Both themes correct; the toggle beats the OS setting in both directions.
- [ ] Palette validator passes on the shipped CSS in both modes (TRK-021).
- [ ] Serves with the application tree entirely absent.
- [ ] No dual-axis chart, no dashed gridline, no series-coloured text anywhere.
