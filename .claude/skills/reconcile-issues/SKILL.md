---
name: reconcile-issues
description: Review one version's already-generated specification/roadmap/implementation/vA.B-issues.md against the REAL current implementation and correct any issue that has drifted (stale file names, changed signatures, evolved contracts, or work already done). Edits the issues file in place with a visible "Reconciled" change-mark. Corrects issues only - never implements code or changes the version.
---

# Skill: Reconcile Issues

For a **pre-generated** issues file, do what `generate-issues` Step 0.5 does for fresh issues:
**ground it in the real, post-fix implementation.** Read the version's
`specification/roadmap/implementation/vA.B-issues.md`, compare each issue's assumptions against the **actual current
code**, and **correct the ones that drifted — in the file, with a visible change-mark** so it's on
record that the original issue was modified.

This skill only **corrects the issues**. It never implements code, never touches the version, and
never uses GitHub. It is the file-driven flow's answer to reconciliation — run it right before
`execute-issues-file`.

## Usage

```
/reconcile-issues <vA.B | path-to-issues-file>
```

- `/reconcile-issues v3.2` → reconciles `specification/roadmap/implementation/v3.2-issues.md`

## Instructions

### Step 0: Read the issues + the real implementation

1. Resolve the target to `specification/roadmap/implementation/vA.B-issues.md` and read it fully (summary table,
   dependency tree, each `### RF-### …` section).
2. Read the **real current code** of the components the issues touch (route by
   [ARCHITECTURE.md](../../../specification/ARCHITECTURE.md) §2) — the actual module/file names, class/method
   signatures, seam contracts, WS event/action names, endpoints, and deps **as they exist now**.
3. Read the prior versions' `specification/roadmap/implementation/*-execution-report.md` and any
   `*-code-review*.md` — especially their **"Fixes applied"** / **"Architecture impact"** notes: those
   record where earlier fixes drifted the code away from the original plan.
4. Read [ROADMAP.md](../../../specification/ROADMAP.md) §`vA.B` (the DoD) + `CLAUDE.md`.

### Step 1: Find the drift

For each issue, compare its **assumptions** against reality:
- Wrong or renamed **file/module** paths (e.g. `server/ws.py` vs the real `server/roboface_server/router.py`).
- **Signatures / names** that changed (methods, fields, the `{move, comment}` shape, event/payload
  keys like `chat_message.sender`, endpoint paths).
- **Contracts** that evolved via a landed fix (a seam whose behavior the code moved past `ARCHITECTURE.md`).
- **Vendor / tech** mismatches (e.g. an issue naming a different model SDK than the shipped seam).
- Work that is **already done** — an issue whose deliverable a prior fix/version already shipped.
- The **code is ground truth** where it disagrees with the issue text (and with a stale spec).

### Step 2: Correct the issues in place, with a visible mark

For each issue that drifted, edit its section in `vA.B-issues.md`:

1. **Fix the details** — the Description / What-needs-to-be-done / Acceptance criteria — so they match
   the real implementation. Keep the **RF id and the intent**; correct only what drifted.
2. **Add a visible change-mark** as a blockquote directly under the issue heading, e.g.:
   > **⟳ Reconciled (<today>):** original referenced `client/agent.py` + `google-genai`; corrected to
   > the shipped `providers/gemini.py` behind the `LLMProvider` seam. Reason: matches the real
   > implementation.
3. If an issue is now **moot** (already delivered), keep it but mark it clearly:
   > **⟳ Reconciled (<today>):** already satisfied by `<commit / version>`; execution is
   > verification-only (add/confirm the test, no new production code).
4. Use today's date in the mark. Never silently rewrite — every change is stamped.

### Step 3: If nothing drifted

If an issue matches reality, leave it untouched (no churn). If **no** issue in the file needed
correction, add a single note under the file's intro — `> **⟳ Reconciled (<today>): no drift found —
issues match the current implementation.**` — and stop.

### Step 4: Record

Commit the corrected issues file (`docs: reconcile vA.B issues against the implementation`, with the
`Co-Authored-By` trailer) and push, so the change-marks persist in history. Report a short summary:
which issues were corrected (and why), which were marked moot, which were untouched.

## Important Rules

- **Correct issues only.** Never implement code, never change the version, never use GitHub — that is
  `execute-issues-file` / `release-version`.
- **The real code is ground truth** where it disagrees with the issue (or a stale spec).
- **Every change is marked.** Preserve the RF id + intent; add a dated `⟳ Reconciled` blockquote for
  each corrected or moot issue. Never rewrite silently.
- **No churn.** Don't touch issues that already match reality.
- **Ask on genuine ambiguity** — if it's unclear whether the issue or the code is "right", surface it
  rather than guessing.
