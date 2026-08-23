---
name: execute-issues
description: Execute GitHub issues for a phase sequentially - implement, validate, commit, push, and generate a report.
---

# Skill: Execute GitHub Issues

Execute GitHub issues for a phase sequentially: implement, validate, commit, push, and
generate a report.

## Usage

```
/execute-issues <label> [--issue RF-###] [--dry-run]
```

The `<label>` is the GitHub phase label exactly as it appears (e.g., `v1.2::phase`).

- `/execute-issues v1.2::phase` -- execute all issues labeled `v1.2::phase`
- `/execute-issues v1.2::phase --issue RF-003` -- execute a single issue from that phase
- `/execute-issues v1.2::phase --dry-run` -- show execution plan without making changes

> [!IMPORTANT]
> **Generate every line fresh.** Every line of code, test, script, and config must be written by the
> executing agent in-session. A complete earlier build of this same spec exists in this repo's git
> history (on `main`, tagged `v6.1.0`). Never `git checkout`, `git cherry-pick`, or otherwise
> recover code from history or any other ref to satisfy an issue — the generated run is the point.

## Instructions

### Step 0: Verify prerequisites

1. Confirm we are on the expected branch (the current working dev branch)
2. Confirm working tree is clean (`git status`)
3. Confirm `gh` is authenticated
4. Parse the label to determine the phase: label `v1.2::phase` -> phase `v1.2`
5. Fetch issues from GitHub:
   ```bash
   gh issue list --label "{label}" --state open --limit 100
   ```
6. Read the phase issues file for detailed descriptions: `specification/roadmap/implementation/v{A.B}-issues.md`
7. If a GitHub report exists (`specification/roadmap/implementation/v{A.B}-github-report.md`), read the RF-to-GitHub# mapping
8. Read [specification/ROADMAP.md](../../../specification/ROADMAP.md) for the version goal and the phase (`vA.B`) DoD/Tests, [specification/ARCHITECTURE.md](../../../specification/ARCHITECTURE.md) for the contracts the issue must honor, and [specification/MISSION.md](../../../specification/MISSION.md) for the product scope (MVP vs later).

### Step 1: Build execution queue

From the GitHub issue list, build an ordered queue based on dependencies:
- Parse RF-### IDs from issue titles (format: `RF-###: {title}`)
- Determine dependency order from the phase issues file dependency tree
- Issues with no unmet dependencies go first
- Closed issues are already excluded (Step 0 fetches `--state open`), so a re-run resumes where the
  last one stopped
- If `--issue RF-###` is specified, execute only that issue (but verify its dependencies are closed)

Show the user the execution plan and ask for confirmation.

### Step 2: Execute each issue (loop)

For each issue in the queue:

#### 2a. Assign and announce

Print: `--- Starting RF-###: {title} ---`

#### 2b. Read issue details

Read the full issue description from the phase issues file (the detailed section for this RF-###).

#### 2c. Implement

Execute the tasks described in the issue. Follow the conventions in `CLAUDE.md` and the
architecture in `specification/ARCHITECTURE.md`. Route by component ([ARCHITECTURE.md](../../../specification/ARCHITECTURE.md) §2):

- **Server** (`server/`): FastAPI + websockets — the pure `protocol` module (message types, `EmotionFrame`, error codes, binary-frame rules), the `router` (connection state machine), the `orchestrator` (streams ASR→LLM→TTS, buffers LLM deltas to clause boundaries), the emotion engine, `providers/` (Gemini LLM, Deepgram ASR, ElevenLabs TTS, each behind its seam with a mock), and from v4 memory/world context over SQLite. **All intelligence lives here** — the device never decides an emotion.
- **Firmware** (`firmware/`): C++ for the M5Stack Core S3 (PlatformIO). Two layers — **pure logic** (header-only, Arduino-free: framing, parsers, state machine, VAD, audio math, lip-sync envelope) host-tested under `pio test -e native`, and **glue** (`namespace app`: M5Unified/M5GFX, WiFi, WS client, audio I/O, camera) validated by compile + on-device smoke. The face renderer + skins live here; the device renders what the server sends and reports what it senses.
- **Assets** (`assets/`): face skin packs (`stackchan/`, `ghost/`, `flame/`, `jelly/`, `cloud/`) + their manifest. A new skin is a shape/palette swap over the same `EmotionFrame` — never renderer logic.
- **Contract changes:** any change to a stable seam — the **WS protocol** (message set, binary frames, error codes), the **`EmotionFrame`** face contract, the **provider seams** (`LLMProvider`/`ASRProvider`/`TTSProvider`), the **`IFaceRenderer` + skin manifest**, or the **capability flags** for board variants — updates `specification/ARCHITECTURE.md` **AND** its contract test, in the same commit.
- Follow existing style/patterns; keep each phase self-contained (don't pull later phases in early — MVP-first, simplicity-first). Use strict typing in Python.

#### 2d. Validate

Run validation checks (Python):

1. **Tests:** `pytest` for the changed packages (unit + the contract tests pinning the seams), where tests exist. An issue touching `firmware/` also runs `pio test -e native` and `pio run -e cores3` (compile) from `firmware/`.
2. **Types:** `mypy server` — strict mode comes from `[tool.mypy] strict = true` in
   `pyproject.toml`. **Do not pass `--config-file mypy.ini`:** no `mypy.ini` is generated, and mypy
   treats a missing config as a hard error (`mypy: error: Cannot find config file 'mypy.ini'`) and
   type-checks nothing — so the gate silently stops being a gate. Pass only the packages that exist
   yet. Fix any error you introduce.
3. **Lint/syntax:** `ruff check server tests`, `python3 -m py_compile {changed_py_files}` and an import check for changed modules.
4. **Contract consistency:** the touched seams match `specification/ARCHITECTURE.md` and their contract tests.
5. **Acceptance criteria:** go through each criterion from the issue and verify against the phase DoD/Tests in `specification/ROADMAP.md`.

Record pass/fail for each check. **Tests are part of the work.** No paid APIs in
validation/CI: the **provider seams are mocked by default** (Gemini/Deepgram/ElevenLabs) and a **fake device** drives the WS protocol; a live call is permitted but opt-in.

#### 2e. Commit

```bash
git add {specific files created/modified}
git commit -m "$(cat <<'EOF'
RF-###: {title}

{1-2 sentence summary of what was implemented}

Closes #{github-issue-number}

Co-Authored-By: <the running model's trailer> <noreply@anthropic.com>
EOF
)"
```

#### 2f. Push

```bash
git push
```

#### 2g. Close issue with summary

```bash
gh issue close {issue-number} --comment "$(cat <<'EOF'
## Implementation Summary

**Commit:** {commit-hash}
**Files changed:** {count}

### What was done
{bullet list of key changes}

### Validation
{pass/fail status for each check}

### Acceptance criteria
{checklist with pass/fail}
EOF
)"
```

#### 2g-bis. Emit tracking events

One line per site, via `python3 -m tracker.emit <type> --emitter skill:execute-issues --scope
phase=..,version=..,step=execute-issues,issue=RF-### [...]`. 2a → `issue.start` (`size`, `area`);
after upload → `issue.uploaded` (`gh_number`, `url`); 2c → `issue.implement.end`; 2d →
`issue.validate.end` (`attempt`, parsed `pytest` and `mypy` counts — on a parse failure emit with
`null` counts and `data.parse_error` rather than skipping the event); 2e → `issue.commit`; 2g →
`issue.closed`; end of loop → `issue.end` (`attempts`).

**Step 3's failure path is the one that matters.** Emit `issue.failed` (with a classified `reason`:
`test-failure` / `type-error` / `import-error` / `timeout` / `other`) and then `issue.reverted`
**before** `git checkout -- .` runs. After the revert there is no commit, no file and no trace — this
event is the *only* record that the attempt happened, which is the whole reason this system exists.

#### 2h. Log progress

Append to the in-memory execution log: issue ID + title, commit hash, files changed,
validation results, status (success/partial/failed).

### Step 3: Handle failures

If implementation or validation fails for an issue:

1. Do NOT commit broken code
2. Revert changes: `git checkout -- .`
3. Add a comment to the GitHub issue explaining what failed
4. Log the failure
5. Ask the user: continue to next issue (if no dependency), or stop?

### Step 3b: No automatic version bump

**Do NOT bump the version automatically.** Never change the version (VERSION file,
RELEASE.txt, or git tag) without explicit user confirmation. When a phase's issues are
all done, report completion and let the user decide whether/when to release via
`/release-version`.

Version notation `A.B.C`: `A` = roadmap version (v0…v6), `B` = phase within it, `C` =
post-release fix. Roadmap phase `vA.B` → release `A.B.0`, tagged `vA.B.0`. If some issues failed or
were skipped, do NOT release — note in the report that the phase is incomplete.

### Step 4: Generate execution report

After all issues are processed (or on stop), generate `specification/roadmap/implementation/v{A.B}-execution-report.md`:

```markdown
# Phase v{A.B} -- Execution Report

**Date:** {date}
**Branch:** {branch name}
**Label:** {label}
**Target release:** v{A.B}.0
**Executed by:** Claude Code

## Summary

| Status | Count |
|--------|-------|
| Completed | {n} |
| Failed | {n} |
| Skipped | {n} |
| Remaining | {n} |

## Issues

| # | RF ID | Title | Phase | Status | Commit | Files | Tests |
|---|----------|-------|-------|--------|--------|-------|-------|
| 1 | RF-001 | ... | v1.2 | completed | a1b2c3d | 4 | pass |

## Detailed Results

### RF-001: ...
**Status:** completed · **Commit:** a1b2c3d
**Validation:** [x] tests · [x] mypy · [x] acceptance

## Next Steps
{remaining issues + dependencies}
```

Commit and push the report (`RF`-style message, with the Co-Authored-By trailer).

## Important Rules

- **Generate every line fresh.** Never `git checkout`/`cherry-pick`/merge code out of git history or any other ref to satisfy an issue — every line is written in-session.
- **One issue at a time.** Never work on multiple issues simultaneously.
- **Dependency order.** Never start an issue whose dependencies are not closed.
- **Clean commits.** Each issue = one commit. No mixing work across issues.
- **No broken code.** Only commit code that passes validation (tests + mypy).
- **Tests ship with the feature.** Mock the provider seams by default (Gemini/Deepgram/ElevenLabs) and drive the protocol with the fake device, so the suite stays deterministic and free; live calls are permitted and opt-in.
- **Intelligence lives on the server.** The device never decides an emotion, never holds persona logic; it renders the `EmotionFrame` it is given and reports what it senses. The only on-device exceptions are the documented local reflexes and the lip-sync envelope.
- **Firmware layering.** Pure logic stays Arduino-free and host-testable; glue modules own the hardware. Don't put parsing or state transitions behind an `M5` include.
- **Contracts stay stable.** A seam change updates `specification/ARCHITECTURE.md` and its contract test in the same commit.
- **Chat is Gemini-only.** The `LLMProvider` seam has exactly one real implementation (Gemini). Never add a second chat vendor.
- **Secrets stay server-side.** `GEMINI_API_KEY`, `DEEPGRAM_API_KEY` and `ELEVENLABS_API_KEY` live only in `server/.env`; the firmware never holds a model key.
- **Ask on ambiguity.** If an issue description is unclear, ask the user rather than guessing.
- **Progress updates.** Print a short status line after each issue completes.
