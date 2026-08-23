---
name: ship-solution
description: Ship the WHOLE solution end-to-end from the already-generated issues files. Takes one selector or a comma-separated LIST of phases/versions/ranges (default: all); the list names TARGETS - missing prerequisite versions are added automatically, de-duplicated, sorted into roadmap dependency order, and already-released versions skipped. Per version - reconcile-issues, execute-issues-file (no GitHub), review-and-fix-issues, release-version A.B.0 - timing each version. HARDEN after each phase by default (--no-harden to skip). At the end, generate a detailed execution report (per-version + per-phase + total statistics and timings). A simplified, file-driven, offline sibling of ship-phase.
---

# Skill: Ship Solution

Build the **entire solution end-to-end from the already-generated issues files** — no issue
generation, no GitHub. It walks every phase's versions in order and, per version, reconciles the
pre-written issues against reality, executes them from the file, reviews-and-fixes, and releases —
**timing each version** — hardens at each phase boundary, and finally **generates a detailed execution
report** with statistics and timings summarized **by version, by phase, and in total**.

This is a simplified, offline sibling of `/ship-phase`. The differences (by ship-phase step):

| ship-phase step | here |
|---|---|
| 0. reconcile (inside generate) | **`reconcile-issues`** — review the *pre-generated* issues file and correct drifted issues **in place, with a `⟳ Reconciled` change-mark** |
| 1. generate-issues | **skipped** — the `vA.B-issues.md` files already exist. **Consequence:** a version with no issues file cannot be built here, where ship-phase would simply generate one (see Step 0.4) |
| 2. upload-issues | **skipped** — no GitHub |
| 3. execute-issues | **`execute-issues-file`** — implement straight from the file (no GitHub, no issue-closing) |
| 4. review-and-fix-issues | **kept** (unchanged) |
| 5. release-version | **kept** (unchanged) |
| end-of-phase HARDEN | **same** — by default after every phase, `--no-harden` to skip |
| per-phase chat report | **replaced** — a single **detailed report** generated after **all** phases |

A **thin orchestrator**: it sequences the sub-skills (`reconcile-issues`, `execute-issues-file`,
`review-and-fix-issues`, `harden-findings`, `release-version`), gates between them, **measures
per-version execution time**, and writes the final report.

> **This pipeline releases.** Invoking `/ship-solution` opts into automated per-version releases (real
> tags + pushes) **and** the per-phase HARDEN sweep. To build without releasing, use the individual
> skills.

## Usage

```
/ship-solution [<selector>[,<selector>…]] [--no-harden]
```

A **selector** is a phase (`vA`), a version (`vA.B`), or a range (`vA-vC`). Pass **one, or a
comma-separated list of any mix** — the whole list is expanded into a single ordered plan. Omit it
entirely to ship the whole solution.

- `/ship-solution` — ship **the whole solution**: every version with a `specification/roadmap/implementation/
  vA.B-issues.md`, in roadmap order, then generate the final report.
- `/ship-solution v3` — phase v3 **and its prerequisites** (v1, v2), hardened per phase, reported
  at the end.
- `/ship-solution v2-v4` — phases v2 through v4, prerequisites filled in.
- `/ship-solution v1,v3,v5` — a **list of phases**, shipped in roadmap order with the gaps filled.
- `/ship-solution v1.1,v1.3,v2` — a **mixed list**: two individual versions plus a whole phase.
- `/ship-solution v3 --no-harden` — no hardening sweeps at any phase boundary.

Whitespace around commas is ignored. `--no-harden` applies to **every** phase in the plan; there is no
per-phase form.

> **The list is a target, not the whole plan.** Missing prerequisites are **added automatically** — the
> roadmap is cumulative, so `/ship-solution v3` plans v1 and v2 as well. Anything already released is
> then skipped, so on a repo built up to v4.1, `/ship-solution v4.2` does exactly one version's
> work. You name the destination; the skill works out what has to happen to get there.
>
> Unlike `/ship-phase`, this skill **cannot generate a missing issues file** — so a required
> prerequisite that has neither a release tag nor an issues file is a hard stop, not a skip (Step 0.4).

## Instructions

### Step 0: Scope, baseline, plan — and start the clock

0. **Check for an unfinished previous run — before anything else.** If `codegen/runs/current` names a
   run with no terminal event, show what it was (command, start time, last released version, what it
   was mid-way through) and **ask**: resume it (same `run_id`, same log, emit `run.resumed`) or start a
   new run (close the old with `run.aborted` `reason: "superseded"`, link the new one via `resumes`)?
   Never decide silently — the choice determines whether this phase's timings belong to one run or two.
1. **Parse the selector list.** Split the argument on commas and trim whitespace; each element is a
   **phase** (`vA`), a **version** (`vA.B`), or a **range** (`vA-vC`). **No argument → all**
   versions that have an issues file. Record whether `--no-harden` was passed (it applies to the whole
   plan). If any element doesn't resolve to a real roadmap phase/version — a typo, an out-of-range
   `v9`, a reversed range (`v3-v1`) — **name it and ask**; never drop it and ship the rest.
2. **Expand to a version set.** Read [specification/ROADMAP.md](../../../specification/ROADMAP.md) and resolve every
   selector to individual versions (`### vA.B` headings under `## vA`, in file order): a phase → all
   its versions; a range → all versions of the phases it spans; a version → itself. **De-duplicate.**
3. **Close the set under its dependencies.** The roadmap is cumulative — v3 (Vision) cannot be built
   without v0's protocol skeleton, v1's voice loop or v2's face. Take the **highest** version in the set and add
   **every roadmap version preceding it** that isn't already there. Missing prerequisites are executed,
   not warned about. **Report the added versions** at confirmation, but don't ask permission — they are
   requirements, not scope creep.
4. **Resolve each version in the closed set to one of three states**, in this order:
   - **Already released** (tag `vA.B.0` exists) → **skip**; the dependency is satisfied. This is what
     keeps the fill cheap: on a repo built to v4.1, `/ship-solution v4.2` still runs one version.
   - **Not released, has `specification/roadmap/implementation/vA.B-issues.md`** → **include** in the plan.
   - **Not released, no issues file** → **STOP.** This skill executes from issues files and cannot
     generate one. Name every version in this state, say plainly that the run cannot proceed because a
     target depends on them, and offer the two real options: run `/generate-issues vA.B` for each (or
     author the files), or use `/ship-phase`, which generates them as step 1. **Never silently drop the
     version and continue** — the target would then build against code its prerequisite never wrote.

   A partially-done version (issues/report exist but no tag) is *not* released: it resumes from its
   remaining steps, since the sub-skills are idempotent.
5. **Sort into roadmap order and group by phase.** The set is a *set*, never a running order —
   `/ship-solution v3,v1` ships v1 first. This is not cosmetic: each version reconciles against the
   previous one's real, released code, so running out of roadmap order would reconcile against a
   codebase that doesn't exist yet. **If the resulting order differs from what was typed, say so.**
6. Confirm the working dev branch + a clean tree; establish a **green baseline** (`pytest` + strict
   `mypy`) and **record the baseline test count** (the "before" for statistics). Never start red.
7. **Start the run clock:** capture `RUN_START=$(date +%s)`. Keep a **running stats table** as you go
   (append each version's row as it finishes to **`.ship-solution-progress.md`** in the repo root — it
   is gitignored — so a long run never loses a measurement).
8. **Size the work** — see **Step 0.5** below. Unlike `/ship-phase`, issue counts here are **counted,
   not estimated**; only duration is projected.
9. **Confirm the plan once** — show the resolved, ordered version list grouped by phase, with the
   dependency fill, any reordering, already-released skips, **and the Step 0.5 sizing**. Then run:
   don't re-confirm each sub-step; pause only for the blockers in the rules.

**Worked example** — `/ship-solution v3.2,v1`, on a repo where v1 is released and every version
has an issues file:

```
selectors : v3.2 · v1
expanded  : v3.2 | v1.1 v1.2 v1.3 v1.4
filled    : + v2.1 v2.2 v2.3 v3.1     ← prerequisites of v3.2, not named by the user
resolved  : v1.1–v1.4 released → skipped
            v2.1–v2.3, v3.1, v3.2 → have issues files → included
ordered   : v2.1 → v2.2 → v2.3 → v3.1 → v3.2

PLAN (5 versions to run)
  v2  v2.1, v2.2, v2.3    → HARDEN → phase row
  v3  v3.1, v3.2            → HARDEN → phase row

ℹ filled in v2.1–v2.3 and v3.1: v3.2 cannot build without them.
ℹ reordered: v3.2 was listed first, ships last — roadmap order is required.
ℹ skipped v1.1–v1.4: already released.
```

Had `v2.2` lacked an issues file, the run would **stop at Step 0.4** rather than skipping it — v3.2
depends on it, and this skill cannot generate the file.

### Step 0.5: SIZE the work — counted here, not estimated

`/ship-phase` has to *estimate* issue counts because `generate-issues` has not run yet. **This skill
does not.** Step 0.4 already established that every planned version has a
`specification/roadmap/implementation/vA.B-issues.md` — otherwise the run stopped — so the issues exist and can simply
be **counted**.

1. **Count issues per version** from each file's Issues Summary Table, and read each issue's **Size**
   (S/M/L) from the same row. Convert to points (S=1, M=3, L=5).
2. **Estimate duration only.** Points × observed mean seconds-per-point from previous runs if any
   exist; otherwise state the assumed rate explicitly so the projection is auditable.
3. Emit **`run.estimate`** with `source: "counted"`, carrying per-version counts, points and the
   duration projection.

> **The burn-down for a `/ship-solution` run therefore has no scope-uncertainty band** — total work is
> known at t=0 and only the *time* axis is projected. That is a real difference from `/ship-phase`, not
> an omission: reconcile-issues may still mark an issue moot, but it never invents new ones.

### Step 1: For each phase → for each version — timed, gated

Run versions **strictly in sequence** — version N+1 only after N is **released** (so N+1's issues
reconcile against N's real, fixed, released code). Invoke each sub-skill via the **Skill tool**.

**At the phase's first version, stamp `PHASE_START=$(date +%s)`.** For **each version**:

- **Stamp `V_START=$(date +%s)`** (before reconcile).
1. **`reconcile-issues vA.B`** — correct the pre-generated issues **in place with a dated `⟳
   Reconciled` mark**; commit the file. (No code implemented here.)
2. **`execute-issues-file vA.B`** — implement each issue from the reconciled file in dependency order
   → validate (`pytest` + strict `mypy`, **LLM mocked**) → commit (one issue = one commit) → push;
   write `vA.B-execution-report.md`. **No GitHub.**
3. **`review-and-fix-issues vA.B`** — the ranked review doc + **fix-now** fixes only (with regression
   tests), recorded in that doc.
4. **`release-version A.B.0`** — bump + tag `vA.B.0` + push.
- **Stamp `V_END=$(date +%s)`** (after release). **Record the version's row:** duration
  `V_END − V_START`, plus the stats collected below.

**Per-version statistics to record** (for the final report):
- **duration** (mm:ss);
- **reconcile:** # issues corrected / marked moot / untouched;
- **execute:** # issues implemented, commit count (or hash range), **tests before → after**;
- **review:** # findings fix-now (fixed) / deferred (by severity);
- **release tag.**

Gate the hand-offs: reconcile → execute → review → release; **release only after the review's fix-now
items are committed and the suite is green**; the **next version only after this one is released**.
Do **not** report to chat between versions.

**Every version boundary ends pushed and clean.** Before starting version N+1, verify `git status` is
clean and there are **no unpushed commits** — `git push` if there are. The sub-skills each push their
own work, so this is a check, not new work; it exists because this skill stops on failure, and a stop
must never strand a version's work locally.

### Step 2: End of every phase — HARDEN (default; `--no-harden` to skip), then close the phase clock

When a phase's last version is released, run the sweep. **This is the default** — invoking
`/ship-solution` is the consent, exactly as it is for the automated per-version releases the same
command performs; the Step 0 plan already included it, so don't ask. **With `--no-harden`, skip it
entirely**: the deferred HIGH/MEDIUM findings stay in their documented homes, and the phase row records
the sweep as skipped with those findings listed as still-outstanding.

Otherwise invoke **`harden-findings vA --release`** — it fixes every still-unfixed 🔴
HIGH / 🟠 MEDIUM finding from the run's code-review reports (each with a regression test), updates those
reports, and ships a **`C` patch** on the phase's latest version (🟡 LOW stays deferred; the escape
hatch still applies). **Stamp `PHASE_END=$(date +%s)`** and record the phase's row: total duration
(`PHASE_END − PHASE_START`), versions, issues, commits, HARDEN findings fixed + patch tag. Then
continue. **No per-phase chat report.**

### Step 3: Generate the final execution report (statistics + timings)

Only after the **whole scope** is shipped (or the run stops), stamp `RUN_END=$(date +%s)` and
**generate `specification/roadmap/implementation/ship-solution-report.md`** — a detailed report with **timings and
statistics summarized by version, by phase, and in total**. Commit + push it, and print its summary to
chat. Structure:

```markdown
# Ship-Solution Execution Report — <date>

## Total
- Wall-clock: <Hh Mm Ss>  (RUN_END − RUN_START)
- Phases: <n> · Versions: <m> · Issues executed: <k> · Commits: <c>
- Releases: <all vA.B.C tags>
- Findings: fix-now fixed <a> · hardened HIGH/MEDIUM <b> · LOW deferred <c> · held <d>
  (with `--no-harden`: hardened 0, and the outstanding HIGH/MEDIUM count with their homes)
- Reconcile: issues corrected <x> · moot <y> · untouched <z>
- Suite: <baseline> → <final> tests passing · mypy --strict clean · zero paid calls

## By phase
| Phase | Versions | Duration | Issues | Commits | Reconciled (corr/moot) | Fix-now | Hardened | Release tags | HARDEN patch |
|-------|----------|----------|--------|---------|------------------------|---------|----------|--------------|--------------|
| vA    | …        | mm:ss    | …      | …       | …                      | …       | …        | …            | …            |

## By version
| Version | Duration | Issues | Commits | Tests (before→after) | Reconcile (corr/moot/kept) | Review (fix-now/deferred) | Release tag |
|---------|----------|--------|---------|----------------------|----------------------------|---------------------------|-------------|
| vA.B  | mm:ss    | …      | …       | … → …                | …                          | …                         | vA.B.00 |

## Timings
- Fastest / slowest version (with durations); average per version; per-phase totals.

## Notes
- Anything held via an escape hatch, anything that stopped early (with what remains), the reconcile
  highlights (notable issue corrections).
```

Durations: compute from the epoch stamps (`end − start`); render `mm:ss` per version, `Hh Mm` for
phases/total. Every number must trace to the run (execution reports, review docs, release tags).

## Important Rules

- **File-driven, no GitHub.** Issues come from `specification/roadmap/implementation/*-issues.md`; nothing is uploaded to
  or closed on GitHub. A required version with **no issues file cannot be built here** — that is a hard
  stop (Step 0.4), never a silent skip, because this skill cannot generate one.
- **Reconcile, don't regenerate.** Step 1 corrects the pre-generated issues in place (with `⟳
  Reconciled` marks), the file-driven analogue of ship-phase's reconcile.
- **Time every version.** Stamp `date +%s` at each version's start/end (and each phase's start/end and
  the run's start/end); persist rows as you go so no measurement is lost.
- **Every version boundary ends pushed and clean** — no unpushed commits, no dirty tree, before the
  next version starts.
- **Release per VERSION**, after its fix-now items are fixed; **next version only after the previous is
  released**. Never batch versions; never release mid-version.
- **The plan is roadmap-ordered and dependency-complete.** A selector list is a *set* of target
  versions, not a running order and not the full scope. De-duplicate, **add every missing version
  preceding the highest selected**, sort into roadmap order, then drop the already-released. The fill is
  reported but not asked about — those versions are requirements.
- **HARDEN runs at each phase boundary BY DEFAULT** and ships a `C` patch; only LOW stays deferred.
  Skipped only with `--no-harden`, and then the outstanding HIGH/MEDIUM findings are recorded as such.
  A fix that can't land cleanly is **held** by `harden-findings`' escape hatch, not forced.
- **One report, at the end** — the generated `ship-solution-report.md` (statistics + timings by
  version/phase/total) plus its chat summary. No per-version or per-phase chat report.
- **Sequential and gated; stop on failure.** Any sub-skill failure or a red `pytest`/`mypy` halts the
  pipeline; report what completed and what remains, and still generate the report for the versions that
  shipped. Never release a version whose suite isn't green.
- **Every fix ships a regression test; the LLM is mocked by default** (live calls opt-in); the suite stays green
  and deterministic.
- **Delegate, never duplicate.** This skill sequences the sub-skills, gates, times, and reports — no
  other logic. Each sub-skill keeps its discipline (one issue = one commit, seam changes carry
  `specification/ARCHITECTURE.md` + contract test, unprefixed `vA.B.C` tags, every line generated fresh).
- **Surface real decisions** — a missing issues file for a required version, an unresolvable selector, a
  tag collision, a held HARDEN finding, an ambiguous reconcile, or any execution/validation failure.
  Routine plan confirmations run straight through.
