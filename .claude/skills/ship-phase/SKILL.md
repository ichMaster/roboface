---
name: ship-phase
description: Full delivery pipeline over the roadmap. Takes one selector or a comma-separated LIST of phases/versions/ranges (e.g. v1,v3.2,v4-v5). The list names TARGETS - missing prerequisite versions are added automatically, the set is de-duplicated, sorted into roadmap dependency order, and already-released versions are skipped. For each phase (vA) run its versions (vA.B) in order - RECONCILE with the real implementation, generate-issues, upload-issues, execute-issues, review-and-fix-issues, release-version A.B.0 (release per VERSION). At the END of every phase a HARDEN sweep runs BY DEFAULT (opt out with --no-harden) fixing the deferred HIGH/MEDIUM findings, then the phase is reported to chat. Gated; stops on failure; surfaces real decisions.
---

# Skill: Ship Phase — the full delivery pipeline

Drive the entire SDLC loop over the roadmap: **phases contain versions; each version is released;
the next version is generated only after the previous one is implemented and fixed** — reconciled
against the real (post-fix) implementation. Hardening of deferred findings runs **by default at every
phase boundary** — pass `--no-harden` to skip it.

> **Terminology (per [ROADMAP.md](../../../specification/ROADMAP.md) §Versioning):** a **phase**
> here is a top-level roadmap block `vA` (v0 Skeleton … v6 FIRE); a **version** is a `vA.B`
> sub-version inside it, released as `vA.B.0`. ⚠️ The roadmap's header and the four sub-skills use
> these words the other way around (they call `vA.B` a "phase" — hence the GitHub label
> `vA.B::phase` and the invocation `generate-issues <vA.B>`). The sub-skill invocations below are
> exactly as they've always been; only this skill's flow description uses the spec's wording.

**The loop:**

```
PLAN = selectors → versions → de-duplicated → + missing prerequisites → sorted into
       roadmap dependency order → minus already-released → grouped by phase

for each PHASE vA in PLAN (in roadmap order):
    for each VERSION vA.B of that phase in PLAN (in order):
        0. RECONCILE   — ground this version in the real implementation + all prior fixes
        1. generate-issues vA.B
        2. upload-issues @specification/roadmap/implementation/vA.B-issues.md
        3. execute-issues vA.B::phase        (implement → validate → commit → push → close)
        4. review-and-fix-issues vA.B        (review → ranked doc → fix-now fixes → same doc)
        5. release-version A.B.0           ← RELEASE PER VERSION (tag vA.B.0)
    → END OF PHASE: HARDEN (skill: harden-findings) — BY DEFAULT (skipped only with --no-harden)
    → REPORT the phase to chat
→ next phase; after the whole scope: overall summary to chat
```

This skill is a **thin orchestrator** — it sequences the sub-skills, adds the reconcile gate and the
end-of-phase hardening sweep, and releases per version; each sub-skill keeps its discipline.

> **This pipeline releases.** Invoking `/ship-phase` is the explicit opt-in to the automated
> per-version releases (real tags + pushes) **and** the per-phase HARDEN sweep. `release-version`'s own
> rules still hold — it never downgrades and confirms the changelog. To build without releasing, use
> the individual skills.

## Usage

```
/ship-phase <selector>[,<selector>…] [--no-harden]
```

A **selector** is a phase (`vA`), a version (`vA.B`), or a range (`vA-vC`). Pass **one or a
comma-separated list of any mix** — the whole list is expanded into a single ordered plan.

- `/ship-phase v2` — ship **phase v2**: every version in it (v2.1 → v2.2 → v2.3), each through
  its five steps incl. its own release; at the phase's end the HARDEN sweep runs; then the phase
  report to chat.
- `/ship-phase v2 --no-harden` — same, but the end-of-phase HARDEN sweep is **skipped**; the deferred
  HIGH/MEDIUM findings stay in their documented homes.
- `/ship-phase v2.2` — ship the single **version v2.2** (steps 0–5, incl. its release), then
  HARDEN v2 at the phase boundary.
- `/ship-phase v2-v3` — ship phase v2, then phase v3 (each hardened at its boundary), then an
  overall summary.
- `/ship-phase v1,v3,v5` — a **list of phases**, shipped in roadmap order.
- `/ship-phase v1.1,v1.3,v2` — a **mixed list**: two individual versions plus a whole phase.
- `/ship-phase v1-v2,v4.1 --no-harden` — a range plus a single version, with no hardening sweeps.

Whitespace around commas is ignored, so `v1, v3` works. `--no-harden` applies to **every** phase in
the plan; there is no per-phase form.

> **The list is a target, not the whole plan.** Missing prerequisites are **added automatically** — the
> roadmap is cumulative, so `/ship-phase v3` plans v1 and v2 as well, and `/ship-phase v1,v3` fills
> in the v2 you left out. Anything already released is then skipped, so on a repo built up to v4.1,
> `/ship-phase v4.2` still does exactly one version's work. You name the destination; the skill works
> out what has to happen to get there.

## Instructions

### Step 0: Scope, baseline, and the phase → version plan

0. **Check for an unfinished previous run — before anything else.** If `codegen/runs/current` names a
   run whose log has no terminal event (`run.end` / `run.aborted`), that run stopped without closing.
   **Show what it was** — its command, when it started, the last version it released, and what it was
   in the middle of — then **ask which this is**:
   - **Resume it** → keep the same `run_id` and keep appending to the same log; emit `run.resumed`.
     The plan is recomputed normally, so already-released versions skip themselves and the run picks up
     where it stopped. Timings stay attributed to one run, with the idle gap excluded (see below).
   - **A new run** → close the old one with `run.aborted` (`reason: "superseded"`), then start fresh
     with `resumes: <old-run-id>` on `run.start` so the two stay linked without being merged.

   Never decide this silently. Resuming when the user meant a fresh run corrupts that run's timings;
   starting fresh when they meant resume splits one phase across two runs and makes "how long did v1
   take" unanswerable. This is exactly the kind of genuine decision this skill pauses for.
1. **Parse the selector list.** Split the argument on commas and trim whitespace. Each element is a
   **phase** (`vA`), a **version** (`vA.B`), or a **range** (`vA-vC`); a single element is just a
   list of one. Record whether `--no-harden` was passed (it applies to the whole plan).
2. **Expand to a version set.** Read [specification/ROADMAP.md](../../../specification/ROADMAP.md) and resolve every
   selector down to individual versions (`### vA.B` headings under `## vA`, in file order):
   a phase → all its versions; a range → all versions of all phases it spans; a version → itself.
   **De-duplicate** — overlapping selectors (`v1,v1.2`) contribute each version once.
3. **Close the set under its dependencies — add every version the plan needs but the user didn't
   name.** The roadmap is strictly cumulative: v3 (Vision) cannot be built without v1's voice loop and
   the protocol skeleton of v0, or v2's face. So take the **highest** version in the set and add **every roadmap version
   that precedes it** and isn't already there. Missing prerequisites are executed, not warned about.
   - `/ship-phase v3` → plans v0.1 … v3.4, not just v3's own four versions.
   - `/ship-phase v1,v3` → the omitted v2.x are filled in.
   - `/ship-phase v4.2` → plans the whole roadmap up to and including v4.2.

   This is safe precisely because of step 7: any filled-in version that is **already shipped** (its
   release tag exists) is skipped, so on a repo that is built up to v4.1, `/ship-phase v4.2` still
   does exactly one version's work. On an empty repo the same command correctly builds everything.
   **Report the added versions** at confirmation — the user gets more than they asked for and should
   see it — but do not ask permission for them; they are requirements, not scope creep.
4. **Sort into roadmap order and group by phase.** The set is a *set*, never a running order.
   `/ship-phase v3,v1` ships v1 first. This is not cosmetic: the pipeline's premise is that each
   version is generated against the previous one's real, released code, so executing out of roadmap
   order would reconcile against a codebase that doesn't exist yet. **If the resulting order differs
   from what was typed, say so** at confirmation. Group the versions back under their phases for the
   per-phase HARDEN and reporting.
5. **Reject nothing silently.** If a selector doesn't resolve to a real roadmap phase/version — a typo,
   an out-of-range `v9`, a reversed range (`v3-v1`) — name the offending element and ask. Never drop
   an unparseable element and proceed with the rest.
6. Confirm we are on the working dev branch and the tree is clean; establish a **green baseline**
   (`pytest` + strict `mypy`). Never start on a red suite — fix a clear flake first or surface it.
7. **Skip already-shipped versions** (release tag `vA.B.0` exists). This is what keeps step 3's
   dependency fill cheap: prerequisites that are already built cost nothing. A version partially done
   (issues/report exist but no tag) resumes from its remaining steps — each sub-skill is idempotent
   (`generate` asks overwrite, `upload` dedupes, `execute` skips closed issues, `release` refuses a
   downgrade).
8. **Estimate the work** — see **Step 0.5** below. Produces an approximate issue count, size mix and
   duration per planned version, so progress has something to be measured against from minute one.
9. **Confirm the plan once** — show it as the resolved, ordered version list grouped by phase, with the
   dependency fill, any reordering, already-shipped skips, **and the Step 0.5 estimate**. Then run: do
   not re-confirm before each sub-step; pause only for the genuine blockers in the rules below.

**Worked example** — `/ship-phase v3.2,v1`, on a repo where v1 is already released:

```
selectors : v3.2 · v1
expanded  : v3.2 | v1.1 v1.2 v1.3 v1.4
de-duped  : (no overlap)
filled    : + v2.1 v2.2 v2.3 v3.1     ← prerequisites of v3.2, not named by the user
ordered   : v1.1 → v1.2 → v1.3 → v1.4 → v2.1 → v2.2 → v2.3 → v3.1 → v3.2
shipped?  : v1.1–v1.4 tagged already → skipped

PLAN (5 versions to run)
  v2  v2.1, v2.2, v2.3    → HARDEN → report
  v3  v3.1, v3.2            → HARDEN → report

ℹ filled in v2.1–v2.3 and v3.1: v3.2 cannot build without them.
ℹ reordered: v3.2 was listed first, ships last — roadmap order is required.
ℹ skipped v1.1–v1.4: already released.
```

Note what the fill did **not** cost: v1 was named by the user but is already shipped, so it drops out;
v2 and v3.1 were never named but are genuinely missing, so they run. The plan is the *work actually
required*, not the literal argument.

### Step 0.5: ESTIMATE the work — before anything is generated

Nothing in the plan yet says *how big* it is. `generate-issues` has not run, so no version's issue count
exists. Without an estimate the burn-down has no total, the ideal line has no endpoint, and the ETA is
blank until the first version finishes — which on a five-version run is a long time to show nothing.

So: **estimate now, from the roadmap alone.** For each version in the plan (skipping already-released
ones), read its `### vA.B` section and estimate three things:

| Estimate | How |
|---|---|
| **Issue count** | Anchor on the version's **Tasks** list — `generate-issues` turns tasks into coherent slices, so a version with N tasks tends toward N issues. Clamp to the 3–7 band that skill produces. Give a low/high, not a point. |
| **Size mix** | From the Tasks + DoD: work touching a seam (WS protocol, `EmotionFrame`, a provider seam, `IFaceRenderer`y) skews **M/L**; additive work inside an existing module skews **S/M**. Convert to points (S=1, M=3, L=5) — the burn-down is size-weighted, so counts alone are not enough. |
| **Duration** | Points × the observed mean seconds-per-point from previous runs if any exist; otherwise state the assumed rate explicitly so the number is auditable rather than magic. |

Emit **`run.estimate`** carrying per-version and total figures, then show them in the Step 0
item 9 confirmation, as a range.

> **⚠️ The estimate must never be given to `generate-issues`.** It is a projection for the burn-down and
> the ETA — not a target, not a quota, and not an input to decomposition. If the estimate reached the
> decomposer, it would become self-fulfilling: the run would produce roughly the predicted number of
> issues and the comparison would measure nothing but its own suggestion. **The estimate and the actual
> are expected to differ, and that difference is a measurement worth having** — it is how well the
> roadmap predicts its own decomposition. Keep them independent so the number stays honest.

When a version is later decomposed, the difference is recorded automatically (`version.decomposed`
carries the real issue list; the reducer compares it to this estimate). A consistent bias in one
direction is a finding about the roadmap or the decomposer, not noise to be tuned away.

### Step 1: For each phase → for each version — the five steps, gated

Run the versions **strictly in sequence** — version N+1 starts only after version N is **released**
(implemented, reviewed, fixed, tagged). That sequencing is the point: the next version's issues are
generated against the previous version's *real, fixed* implementation. Invoke each sub-skill via the
**Skill tool** (it loads that skill's instructions; follow them fully).

**0. RECONCILE** — the first act of every version's cycle, carried out **inside `generate-issues`
   (its Step 0.5)**: before decomposing `vA.B`, read (a) the **real current code** of the components
   it touches, (b) prior `specification/roadmap/implementation/*-execution-report.md`, and (c) prior
   `specification/roadmap/implementation/*code-review*.md` — especially their **"Fixes applied"** and **"Architecture
   impact"** notes from review/harden work. Where fixes drifted the code from `ARCHITECTURE.md`, the
   implementation is ground truth; doc corrections ride along in the seam-touching issue. This is
   where "changes in architecture after fixes" enter the next version's issues.
1. **`generate-issues vA.B`** → `specification/roadmap/implementation/vA.B-issues.md` (reconciled, per step 0).
2. **`upload-issues @specification/roadmap/implementation/vA.B-issues.md`** → the GitHub issues + labels + deps +
   `vA.B-github-report.md`.
3. **`execute-issues vA.B::phase`** → implement → validate → commit → **push** → close each issue in
   dependency order (statuses change; one issue = one commit), then `vA.B-execution-report.md`.
4. **`review-and-fix-issues vA.B`** → code review, the criticality-ranked recommendations doc, the
   **fix-now** fixes only (with regression tests, LLM mocked), results recorded **in that same doc**
   (incl. "Architecture impact" notes). Deferred findings stay deferred — they are the HARDEN sweep's
   input, at the end of the phase, if the user opts in.
5. **`release-version A.B.0`** → bump `VERSION`/`RELEASE.txt`/the app version, tag `vA.B.0`
   (unprefixed — this repo is standalone and nothing collides), and push. **Release per version.**

Gate the hand-offs: upload only after generate wrote the file; execute only after the issues exist;
review only after execute closed the issues with a green report; **release only after the review's
fix-now items are committed and the suite is green**; the **next version only after this one is
released**.

**Every version boundary ends pushed and clean.** Before starting version N+1, verify `git status` is
clean and the branch has **no unpushed commits** — `git push` if it does. Each sub-skill already pushes
its own work (one issue = one commit = one push; review pushes each fix; `release-version` pushes the
branch and its tag), so this is a check rather than new work — but it is the check that makes the
guarantee real. This skill **stops on failure by design**, so halting mid-version is a normal outcome,
not an edge case: a stop must never strand a version's work on one machine.

### Step 1.5: TRACKING — emit as you go

Emit one event per transition: `python3 -m tracker.emit <type> --emitter skill:ship-phase --scope
k=v,... [--status ok|fail|skip] [--data '{...}']`. It never raises and never blocks, so a tracking
failure cannot fail a step (architecture §5.2). Sites: Step 0 → `run.start` (plan, baseline, git), or
`run.resumed` when resuming; Step 0.5 → `run.estimate`; per phase → `phase.start`/`phase.end`; per
version → `version.start`/`version.end`/`version.skipped`; per sub-skill → `step.start`/`step.end`; a
blocked gate → `gate.blocked`; the end → `run.end`. `--no-harden` still emits `harden.skipped` — a
missing event and a skipped sweep must never look alike.

### Step 2: END OF PHASE — HARDEN (default; `--no-harden` to skip)

When the phase's last version is released, sweep the deferred 🔴 HIGH / 🟠 MEDIUM findings accumulated
in the run's code-review reports. **This runs by default** — invoking `/ship-phase` is the consent,
exactly as it is for the automated per-version releases the same command performs.

- **No flag** → run the sweep. Do not ask; the plan confirmed at Step 0 already included it.
- **`--no-harden` was passed** → skip it entirely. The deferred HIGH/MEDIUM findings stay in their
  documented homes (e.g. v5.1) and are listed as still-outstanding in the phase report.

> Leaving hardening on by default means a phase does not close with known HIGH-severity findings
> sitting unfixed in its own review docs. The escape hatch inside `harden-findings` — a fix that can't
> land cleanly is held with a reason rather than forced — is what keeps that safe.

**Delegate the sweep to the dedicated skill:** invoke **`harden-findings vA --release`** via the
Skill tool and follow its instructions fully. That skill collects the run's
code-review reports, fixes every still-unfixed 🔴 HIGH / 🟠 MEDIUM finding (🟡 LOW stays deferred) —
each with a regression test, validated green, one focused commit — updates the reports in place
("Fixes applied" + "Architecture impact", which the next phase's RECONCILE reads), and, because the
phase's versions are already released, ships the result as a **`C` patch release** on the phase's
latest version (e.g. `v2.3.01`). Its escape hatch (a fix that can't land safely is held with a
reason and surfaced) applies unchanged.

### Step 3: REPORT the phase to chat

After the phase (and its HARDEN sweep, unless `--no-harden`), **report the phase to chat** (not a file):
- **Per version:** RF id range → GitHub #s, execution commit range + test/typing status, review
  finding counts (**fixed-now / deferred**, with homes), any **Architecture impact** deltas, and the
  release tag.
- **HARDEN outcome:** which findings were fixed and the patch tag — or, with `--no-harden`, that the
  sweep was skipped, plus the still-outstanding HIGH/MEDIUM findings and their homes. Findings **held**
  by the escape hatch are listed either way, with the reason.
- **Phase rollup:** what the phase delivered against its roadmap goal.

Then continue to the next phase. After the whole scope, add a short **overall summary** (phases
shipped, versions skipped as already-released, anything stopped early and what remains, what's next).

## Important Rules

- **Release per VERSION (`vA.B.0`)** — after that version is built, reviewed, and its fix-now items
  fixed. Never batch several versions into one release; never release mid-version.
- **HARDEN is end-of-phase and runs BY DEFAULT.** Invoking `/ship-phase` is the consent. It is skipped
  only when `--no-harden` was passed — then the deferred HIGH/MEDIUM findings stay in their documented
  homes and are surfaced as outstanding in the phase report. When it runs and lands fixes, ship them as
  a `C` patch release on the phase's latest version. A fix that can't land cleanly is **held** by
  `harden-findings`' escape hatch, not forced.
- **Every version boundary ends pushed and clean.** No unpushed commits, no dirty tree, before the
  next version starts. This skill stops on failure by design, so a stop must never leave a version's
  work on one machine only.
- **Next version only after the previous is released.** The strict sequencing is what makes the
  RECONCILE step meaningful: version N+1's issues are generated against version N's real, fixed code.
- **Reconciliation is step 0 of every version** (via `generate-issues` Step 0.5): real code + execution
  reports + review docs' "Fixes applied"/"Architecture impact" are the input to the next version's
  issues; `ARCHITECTURE.md` corrections ride along in seam-touching issues.
- **Sequential and gated.** Each step's output is the next step's input. Never start a step whose
  predecessor didn't finish cleanly; never interleave two versions' pipelines.
- **Stop on failure — do not paper over it.** If any sub-skill fails, or any fix hits a red
  `pytest`/`mypy`, halt, report what completed and what remains, and let the user decide. Never release
  a version whose suite isn't green.
- **Every fix ships a regression test**, the LLM is mocked by default (live calls opt-in), and the suite stays
  green and deterministic.
- **Surface real decisions.** Pause for an **ID/tag collision**, an **overwrite/append** prompt, a
  **held** HARDEN finding, or any execution/validation failure. Routine plan confirmations run straight
  through — and the HARDEN sweep is no longer one of them, since the Step 0 plan already covered it.
- **Delegate, never duplicate.** This skill only sequences the sub-skills (`generate-issues`,
  `upload-issues`, `execute-issues`, `review-and-fix-issues`, `harden-findings`, `release-version`)
  and adds the gating; no logic of its own. Each sub-skill keeps its discipline — one issue = one
  commit, seam changes carry `specification/ARCHITECTURE.md` + contract test, IDs stay in this branch's
  `RF-###` namespace, releases use unprefixed `vA.B.C` tags, every line generated fresh.
- **Ask on a bad target.** If **any** element of the selector list doesn't resolve to a real roadmap
  phase/version, name that element and ask — never silently drop it and ship the rest.
- **The plan is roadmap-ordered and dependency-complete, always.** A selector list is a *set* of target
  versions, not a running order and not the full scope. De-duplicate it, **add every missing version
  that precedes the highest one selected**, sort into roadmap order, then drop the already-shipped.
  Surface the fill, the reordering, and the skips at confirmation — but the fill is not optional and is
  not asked about: those versions are requirements. Honoring a user-supplied order, or executing a plan
  with dependency holes, would break the reconcile premise that each version builds on the last one's
  real released code.
