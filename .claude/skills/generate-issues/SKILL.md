---
name: generate-issues
description: Decompose a roadmap phase into a per-phase GitHub-issues file at specification/roadmap/implementation/, ready for /upload-issues.
---

# Skill: Generate Version Issues

Decompose one ROADMAP **phase** (`vA.B`) into a fine-grained, dependency-ordered
**issues file**, written to `specification/roadmap/implementation/`. The output is the input to
`/upload-issues` (which pushes it to GitHub) and then `/execute-issues` (which
implements it).

## Usage

```
/generate-issues <phase>
```

- `/generate-issues 1.2` — decompose ROADMAP phase **v1.2** → `specification/roadmap/implementation/v1.2-issues.md`
- `/generate-issues v2.1` — phase **v2.1** → `…/v2.1-issues.md`

One file per **phase** (`vA.B`). IDs (`RF-###`) are **globally sequential** and
continue across phase files **and across regeneration runs** — never reset.

## Instructions

### Step 0: Read inputs

1. Normalize the phase to `vA.B` (no zero padding, e.g. `1.2` → `v1.2`, `v2.10` stays `v2.10`).
2. Read [specification/ROADMAP.md](../../../specification/ROADMAP.md) §`vA.B` — the phase's **Goal**,
   **Tasks**, **DoD**, and **Tests** (and the version heading it sits under).
3. Read [specification/ARCHITECTURE.md](../../../specification/ARCHITECTURE.md) for the contracts and
   components the phase touches, and [specification/MISSION.md](../../../specification/MISSION.md)
   for the product vision and scope (MVP vs later).
4. Read `CLAUDE.md` for code conventions, the module map, and the non-negotiable seams.
5. **Find the next free `RF-###` id — never restart the numbering.** Ids are globally
   sequential across every phase *and every regeneration run*. Check **both** sources and
   continue from whichever is higher:

   ```bash
   # (a) GitHub — the durable source: issues survive a wiped working tree
   gh issue list --state all --limit 1000 --json title \
     --jq '.[].title | capture("RF-(?<n>[0-9]+)").n' 2>/dev/null | sort -n | tail -1

   # (b) local issues files — covers ids drafted but not yet uploaded
   grep -rhoE 'RF-[0-9]+' --include='*-issues.md' specification/roadmap/implementation/ 2>/dev/null \
     | grep -oE '[0-9]+' | sed 's/^0*//' | sort -n | tail -1
   ```

   (Pass the directory with `--include`, not a `*-issues.md` glob — under zsh an unmatched glob
   aborts the command before `2>/dev/null` can swallow it, and `specification/roadmap/implementation/` may not exist.)

   The next id is **`max(a, b) + 1`**, zero-padded to three digits (`RF-001`, `RF-047`,
   `RF-118`). Start at `RF-001` **only if both sources come back empty**.

   > `specification/roadmap/implementation/` is cleared between regeneration runs, so scanning it alone would
   > silently restart the sequence at 001 and collide with ids already on GitHub. That is why (a)
   > is checked first and the two are combined rather than either being trusted on its own. If
   > `gh` is unauthenticated or the repo has no issues, (a) yields nothing and (b) governs — say
   > so in the report rather than silently assuming the sequence is fresh.
6. If `…/v{A.B}-issues.md` already exists, ask whether to overwrite or append.

### Step 0.5: Reconcile with the real implementation

Before decomposing, ground the new `vA.B` in **what was actually built and fixed** — not just what
`ARCHITECTURE.md` describes. Earlier `vA.B`s may have drifted the code from the docs via review
fixes and hardening; the new issues must build on reality. This step is the **reconciliation after
fixes**: it always runs right after the previous `vA.B` was implemented + fixed (+ released, in the
`/ship-phase` pipeline), and its inputs are the real code plus the fix records listed below.

1. For the components this phase touches (route by [ARCHITECTURE.md](../../../specification/ARCHITECTURE.md) §2),
   read the **real current code** — the actual seams, method signatures, and behaviors as implemented,
   not only the design in `ARCHITECTURE.md`.
2. Read the completed phases' `specification/roadmap/implementation/v*-execution-report.md` and any
   `specification/roadmap/implementation/*code-review*.md` — especially their **"Fixes applied" / "Architecture impact"**
   notes — to see what changed during implementation, review, and hardening.
3. **Reconcile:** if `ARCHITECTURE.md` is stale relative to a landed fix (a seam or contract evolved),
   treat the **real implementation as ground truth** for this phase's issues, and note the drift. If a
   contract genuinely changed but the doc wasn't updated, flag it and prefer correcting `ARCHITECTURE.md`
   in the seam-touching issue (with its contract test). Decompose against the **actual** current
   contracts so the new issues don't re-assume a design the code has already moved past.

### Step 1: Decompose the phase

Turn the phase's **Tasks** into a small set of issues (typically **3–7**), each a
coherent, independently shippable slice:

- Size each **S** (1–2 d) / **M** (3–5 d) / **L** (5–8 d).
- Order by dependency; the first issue is usually the **gate** (the seam/structure
  everything else builds on).
- Map each issue to part of the phase Tasks; together they must satisfy the phase **DoD**.
- **Bake tests into every issue** (the LLM is mocked by default; live calls are opt-in): unit for
  pure logic, contract for any seam, an integration turn where relevant.
- A seam change — the **WS protocol**, the **`EmotionFrame`** face contract, a **provider seam**
  (`LLMProvider`/`ASRProvider`/`TTSProvider`), the **`IFaceRenderer` + skin manifest**, or the
  **capability flags** — carries a `specification/ARCHITECTURE.md` update + its contract test in the
  **same** issue.
- Stay **within the phase** — don't pull later phases' scope in early (each version is self-sufficient;
  hardware and features are assigned to a version on purpose).

### Step 2: Write the issues file

Write `specification/roadmap/implementation/v{A.B}-issues.md` using **exactly** this format:

````markdown
# v{A.B} — GitHub Issues

Issues for phase **v{A.B} — {phase title}** (version **v{A} — {version title}**),
derived from the per-phase Tasks in [ROADMAP.md](../../ROADMAP.md) (§v{A.B}) and the
contracts in [ARCHITECTURE.md](../../ARCHITECTURE.md) ({the relevant § sections}).
This file is scoped to a single phase; IDs continue from the previous phase
(RF-{prev} → **RF-{first}…{last}**).

{1–3 sentences: what the phase does, the seams it extends, why now.}

## Issues Summary Table

| # | ID | Title | Size | Area | Phase | Dependencies |
|---|----|-------|------|------|-------|--------------|
| 1 | RF-{first} | {title} | M | {server/firmware/assets/tests} | v{A.B} | -- |
| 2 | RF-{…} | {title} | S | {area} | v{A.B} | RF-{first} |
| … | … | … | … | … | … | … |

**Size legend:** S = 1–2 days, M = 3–5 days, L = 5–8 days

---

## Dependency Tree

```
RF-{first} ({gate})
  |
  +-- RF-{…} (…) --+
  |                   |
  +-- RF-{…} (…) --+
                      |
             RF-{…} (…)  => {phase DoD}
```

**Parallelization hints:** {which gate first; what runs in parallel after}.

---

## v{A.B} — {phase title}

### RF-{id} — {Title}

**Description:**
{1–3 sentences. Note which module(s) it touches: server/ firmware/ assets/ tests/.}

**What needs to be done:**
- {bullet}
- {bullet}

**Dependencies:** {RF-ids, or None}

**Expected result:**
{one sentence}

**Acceptance criteria:**
- [ ] {functional criterion}
- [ ] **Contract test:** {seam pinned} — *(only if a seam changes)*
- [ ] **Unit test:** {pure logic} with the **LLM mocked by default** (live calls opt-in)
- [ ] {ties to the phase DoD}

---

{repeat the `### RF-{id} …` block per issue}

## v{A.B} scope notes

**Total effort:** {rough estimate}.
**Critical path:** RF-{…} → … → RF-{…}.
**Phase DoD (roadmap §v{A.B}):** {restate the DoD}.
**Contracts pinned this phase:** {the seams + their tests}.
**Model note:** chat runs on **Gemini 2.5 Flash only** (`thinkingBudget: 0`) behind the `LLMProvider`
seam; **no paid APIs** — every provider seam is mocked and a fake device drives the protocol for unit +
integration tests.
**Companion documents:**
- [ROADMAP.md](../../ROADMAP.md) — version goals, per-phase Tasks/DoD/Tests (§v{A.B}).
- [ARCHITECTURE.md](../../ARCHITECTURE.md) — {the relevant § sections}.
- Generated on upload: `v{A.B}-github-report.md` (RF-### → GitHub #), then `v{A.B}-execution-report.md`.
````

### Step 2.5: Emit `version.decomposed`

Once the issues file is written, emit `version.decomposed` with every issue id and its `size`:
`python3 -m tracker.emit version.decomposed --emitter skill:generate-issues --scope
phase=vA,version=vA.B --status ok --data '{"issues":[{"id":"RF-001","size":"M"},...]}'`.

This is the instant total scope changes (architecture §3.2). Everything that shows progress —
burn-down, ETA, issues done — depends on distinguishing before from after it.

**Do not read any estimate before decomposing.** The orchestrator's `run.estimate` is a projection for
the burn-down; if it reached this skill the decomposition would be told how many issues to produce, and
the estimate-versus-actual comparison would measure nothing but its own suggestion.

### Step 3: Report

Show the user: the file path, the issue count, the `RF-###` id range, and the
critical path. Suggest the next step:

```
/upload-issues @specification/roadmap/implementation/v{A.B}-issues.md
```

(Do **not** create GitHub issues here — that's `/upload-issues`. This skill only writes
the local issues file.)

## Important Rules

- **One file per phase** (`vA.B`) at `specification/roadmap/implementation/v{A.B}-issues.md`.
- **IDs are globally sequential** (`RF-###`), continuing across phase files **and across
  regeneration runs** — never reset. Resolve the next id from `max(GitHub, local issues files) + 1`
  (Step 0.5); starting at `RF-001` is correct only when both sources are genuinely empty.
- **Tests in every issue.** Acceptance criteria include the unit/contract/integration tests; the LLM is mocked by default; live calls are opt-in.
- **Seam = ARCHITECTURE + test together.** Any contract change lands its `specification/ARCHITECTURE.md` update and contract test in the same issue.
- **Scope to the phase.** Map issues to the phase's Tasks/DoD; don't pull later phases in early (MVP-first, simplicity-first).
- **Honor the DoD.** The issues together must satisfy the phase DoD in roadmap §v{A.B}.
- **Ask on ambiguity.** If the phase's Tasks are unclear or under-specified, ask the user before inventing scope.
- **Don't touch GitHub.** This skill writes only the local file; `/upload-issues` pushes it.
