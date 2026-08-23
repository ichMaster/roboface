---
name: review-and-fix-issues
description: Code-review a release or the current branch, write a criticality-ranked recommendations doc in specification/roadmap/implementation/, implement the fix-now items with regression tests, then record what was done in the SAME doc. Never releases.
---

# Skill: Review & Fix Issues

Run one loop over a codebase: **review → recommend → fix → record.** It (1) performs a critical code
review, (2) writes a single recommendations document ranking findings by criticality and marking
each **FIX NOW** or **DEFER →**, (3) implements the fix-now items with regression tests, and (4)
**updates that same document in place** — marking what was fixed and adding a "Fixes applied"
section. The recommendations and the results live in **one document**.

This skill fixes only the small, in-scope, high-value findings. It **never** bumps the version or
cuts a release (that stays `/release-version`, explicit), and it never pulls deferred/larger work
forward without flagging it.

## Usage

```
/review-and-fix-issues [target]
```

- `/review-and-fix-issues v1.4` — review the released v1 phase/version (through its tag).
- `/review-and-fix-issues` — review the **current branch / working tree** (everything built so far).
- `/review-and-fix-issues firmware` — scope the review to one component (`server`/`firmware`/`assets`).

## Instructions

### Step 0: Scope + green baseline

1. Resolve `target`: a version/phase (`vA`/`vA.B`) or its tag; a component (`server`/`firmware`/
   `agent`/`web`); or, with no argument, the **current branch** (the whole codebase).
2. Confirm we are on the working dev branch and the tree is clean.
3. **Establish a green baseline** — run `pytest` + `mypy` (strict). If the suite is **red or flaky**,
   say so: a review on a red baseline is unreliable. Fix a clear flake first (small, focused, its own
   commit) or surface it and ask before continuing. **Never review or fix on top of a red suite.**

### Step 1: Critical code review

Read the in-scope modules — route by [ARCHITECTURE.md](../../../specification/ARCHITECTURE.md) §2 — and focus
on the highest-risk seams first: the WS protocol and the router's connection states (§Contracts, §Turn lifecycle), the
the WS protocol and `EmotionFrame` (§Contracts), the provider seams, and the firmware pure/glue split.

Be **adversarial** — hunt for *real* defects, not restatements of what works:
- **Concurrency:** races at `await` points, read-then-write without the DB guard handled, async
  cancellation of cleanup (the §10 finally path), interleaving of two connections.
- **Authority:** emotion or persona logic leaking into the firmware; a device claim trusted without
  server-side validation; state kept on the device that belongs on the server.
- **Correctness:** turn-state edge cases (a turn cancelled mid-stream, `tts_end` after cancellation),
  `ttl_ms` expiry and crossfade interaction, frame ordering, VAD/endpointer boundaries, off-by-one in
  audio buffers.
- **Robustness:** malformed input, dropped sockets, unhandled exceptions that kill a connection.
- **Input hygiene & secrets:** unvalidated frames from the device (oversize audio/JPEG, frames out of
  state); a model key reaching the firmware or any log; anything a test would never call for real.
- **Seam drift:** code that diverges from the pinned contracts or the `MISSION.md` scope.

For each finding, capture: a **concrete failure scenario** (inputs → wrong result/crash), a
`file:line` anchor, a **severity** (🔴 HIGH / 🟠 MEDIUM / 🟡 LOW), and a **proposed fix**. Cross-check
findings against the specs; if a gap is *already scheduled* for a later phase (e.g. v5.1), note that
rather than treating it as new.

### Step 2: Write the recommendations document (the plan)

Write **one** doc at `specification/roadmap/implementation/<scope>-code-review.md` (e.g. `v1.4-code-review.md`, or
`branch-code-review.md` for the working tree). Include:

- A header: date, reviewer, **scope**, method.
- A **criticality-ranked summary table** with columns: `# | Severity | Finding | Recommendation |
  Status`. **Recommendation** is `FIX NOW` or `DEFER → <home>`; **Status** starts as blank/pending.
- Per-finding detail: the failure scenario + the proposed fix.
- A short **"What's solid"** section (keep the review balanced).
- **Suggested next actions.**

Decide **FIX NOW vs DEFER** honestly:
- **FIX NOW** = real, small, self-contained, high-value, and in-scope now (e.g. a crash-causing race,
  an authority-rule violation, trivial input validation).
- **DEFER →** = larger resilience work, or anything already owned by a later roadmap phase — give the
  home (`v5.1`, `v2.3`, "cleanup/`/simplify`", "documented MVP scope"). Do **not** pull these
  forward.

Commit the doc as the plan (a `docs:` commit) **and push it**. The review is worth keeping even if the
fix pass is interrupted.

### Step 3: Implement the FIX-NOW items (with tests)

For each **FIX NOW** finding, in criticality order:

1. Implement the fix following `CLAUDE.md` + `specification/ARCHITECTURE.md`. Keep it minimal and in-scope.
2. **Add a regression test that would have caught the bug** (e.g. a concurrent-path test for a race).
   The **LLM is mocked by default**; live calls are opt-in.
3. **Validate:** `pytest` (green, deterministic), `ruff check server tests` + `mypy server` (strict);
   firmware fixes also `pio test -e native`. Only commit code that passes.
4. **Commit** one focused change per finding, referencing the finding number
   (`fix(<area>): … (code review #N)`), with the `Co-Authored-By` trailer — **then `git push`**, the
   same per-unit discipline `execute-issues` applies to issues. Never leave a landed fix unpushed.
5. **Seam changes** (WS protocol / `EmotionFrame` / a provider seam / `IFaceRenderer`) update
   `specification/ARCHITECTURE.md` **and** the contract test in the **same** commit.

If a fix turns out larger than "fix now" (touches a seam broadly, or needs design), **stop, re-classify
it to DEFER** in the doc with the reason, and move on — don't half-land it.

### Step 4: Update the SAME document (the result)

Edit the doc **in place**:
- Flip the **Status** column to `✅ FIXED — <commit>` for each applied fix (and keep `⏳ deferred`
  for the rest).
- Add a **"Fixes applied"** section: per fix, the change, the regression test, and the verification
  (final `pytest` + `mypy` status). For any fix that **changed a documented contract or a
  design-relevant behavior**, add an explicit **"Architecture impact"** note (what changed vs the
  original design), and ensure `specification/ARCHITECTURE.md` reflects contract changes. This record is what a
  later `/generate-issues` reconciles the next version against — so the next version builds on what was
  really implemented, not the stale design.
- Update **"Suggested next actions"** (e.g. `/release-version` for a patch on a released phase; carry
  deferred items into their phase).

Commit the doc update (a `docs:` commit) **and push**. This skill must leave nothing unpushed — it is
followed by `release-version`, but a run that stops here would otherwise strand every fix locally.

### Step 4.5: Emit tracking events

Per finding, via `--emitter skill:review-and-fix-issues --scope phase=..,version=..`: Step 2 →
`finding.raised` (`finding`, `severity`, `title`) then `finding.classified` (`disposition`); Step 3 →
`finding.fixed` (`sha`, **plus `pytest` with the full-suite counts after the fix**) for each fix-now
item; deferred ones → `finding.deferred` (`home`).

**Carry the suite size on every `finding.fixed`** — `--data '{"finding":"…","sha":"…","pytest":
{"passed":100,"failed":0}}'`. A fix changes the suite, and `tests_passing` is otherwise frozen at
whatever the last issue reported, so the dashboard disagrees with the repo from the first fix onward.
Report a **full-suite** run, never a single file's count.

Counts must match this document's own summary table — the review doc and the log are two records of
one review, and a discrepancy between them means one of the two is wrong.

### Step 5: Report

Summarize: findings by severity; which were **fixed** (with commits) and which **deferred** (with
homes); the final green suite + strict-mypy status. If fixes landed on an already-released phase,
suggest `/release-version <phase>.C` (the `C` patch) — but do **not** run it. Offer a deeper
adversarial pass (`/code-review ultra`) for confirmation.

## Important Rules

- **One document, updated in place.** The recommendations and the results share a single doc — mark
  what was fixed and add the "Fixes applied" section rather than writing a new file.
- **Fix only the fix-now items.** Never pull deferred or larger work forward without re-classifying and
  explaining it in the doc.
- **Every fix ships a regression test**, and the LLM is mocked by default — no paid API call in any test.
- **Green before, green after.** Establish a green baseline; only commit code that passes `pytest` +
  strict `mypy`; keep the suite deterministic.
- **Record architecture deltas.** A seam/contract change updates `specification/ARCHITECTURE.md` and its contract
  test in the **same** commit. Any fix that alters documented behavior gets an **"Architecture impact"**
  note in the review doc — so the next `/generate-issues` can reconcile the following version against
  what was really built, not the stale design.
- **Never release.** No version bump, no tag — recommend `/release-version` and stop.
- **Never leave work unpushed.** Every commit this skill makes is pushed immediately. It stops on a red
  suite or a re-classified finding, so "the next step will push it" is not a safe assumption.
- **Generate every line fresh.** Never `git checkout`/`cherry-pick`/merge code out of git history or any other ref.
- **Ask on genuine ambiguity** — an unclear scope, or a borderline finding where fix-now vs defer is a
  real judgment call the user should make.
