# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current state: spec-first, no product code yet

`server/`, `firmware/`, `assets/` and `tests/` do not exist — they are the output the SDLC skills generate from the specification. What exists is the specification, the imported skill pipeline, and the generation tracker. There is therefore **no product build/test command yet**; the only working suite is `codegen/`'s own (below).

```
specification/   MISSION.md · ARCHITECTURE.md · ROADMAP.md   ← the source of truth, English only
  CONCEPT.md / CONCEPT-en.md   the original concept it was derived from
  face-prototype.html          the executable face-renderer reference
  roadmap/implementation/      issues files + execution/review reports the skills write
.claude/skills/  eleven SDLC skills that generate this repo from the specification
codegen/         generation tracking: event log, hooks, reducer, dashboard (:8420)
```

**Read the specification before writing any code.** It settles the hardware map, the wire contracts, the version ladder and the decisions already taken:

- [specification/MISSION.md](specification/MISSION.md) — what RoboFace is, the principles, the non-goals, the glossary.
- [specification/ARCHITECTURE.md](specification/ARCHITECTURE.md) — components, contracts, turn lifecycle, the face, interaction, audio, vision, the mind, seams, data model, testing, repo layout.
- [specification/ROADMAP.md](specification/ROADMAP.md) — v0–v6 as dotted phases `vA.B`, each with Goal / Tasks / DoD / Tests.
- [specification/face-prototype.html](specification/face-prototype.html) — the executable reference for the face renderer: `renderFace(skin, frame)` is pure, five skins share one expression grammar, and the JSON under the screen is exactly what the server sends. Port that structure to M5GFX sprites; don't invent a second one.

**All specification documents are English.** `specification/CONCEPT.md` (Ukrainian) and its English twin are the original concept, kept as history; where they and the three specs disagree, the specs win.

## What RoboFace is

A desktop AI companion with a living animated face on an **M5Stack Core S3**. Three tiers:

```
Core S3 firmware (face + audio I/O + camera + sensors)
   ⇅ WiFi / WSS: JSON control frames + binary PCM16 + binary JPEG
Python server (protocol · router · orchestrator · emotion engine · vision · mind)
   ⇅ HTTPS
Gemini 2.5 Flash (chat + vision) · Deepgram nova-2 (ASR, uk) · ElevenLabs (TTS)
```

**The device is thin by design.** All intelligence lives on the server; the firmware renders the `EmotionFrame` it is given and reports what it senses. Two deliberate local exceptions, both for latency: **lip-sync** (from the amplitude envelope of the TTS audio the device is already playing — the server sends nothing for it) and **reflexes** to touch/IMU/proximity (<100 ms of animation, never interrupting speech or listening).

## Rules that constrain implementation

- **Chat is Gemini, and only Gemini.** `LLMProvider` has exactly one real implementation — Gemini 2.5 Flash with `thinkingBudget: 0` — covering chat, the vision turn and the background emotion read. A second chat vendor is a non-goal, not a backlog item. ASR (Deepgram) and TTS (ElevenLabs) are separate seams; speech is not chat.
- **No emotion decision on the device.** The server decides; the device renders. Turn states are expressed as faces, never as text labels on the screen.
- **Skins are asset swaps.** All five faces (procedural Stack-chan + ghost/flame/jelly/cloud) render the same `EmotionFrame`. If a skin needs renderer logic, the design is wrong.
- **Firmware layering.** Arduino-free pure logic (framing, state machine, VAD, lip-sync envelope, gaze math, recipes) is host-tested under `pio test -e native`; glue modules own the hardware. Parsing and decisions never sit behind an `M5` include.
- **Five seams, each pinned by a contract test:** the WS protocol, `EmotionFrame`, the provider seams, `IFaceRenderer` + the skin manifest, and the `caps` capability flags. A seam change updates `specification/ARCHITECTURE.md` **and** its contract test in the same commit.
- **No paid APIs in tests.** A fake device drives the protocol and every provider is mocked; a live call is opt-in and never the default suite.
- **Secrets server-side.** `GEMINI_API_KEY`, `DEEPGRAM_API_KEY`, `ELEVENLABS_API_KEY` live in `server/.env`. The firmware holds only WiFi credentials and the server URL, in a gitignored `firmware/src/config.h`.
- **Build in roadmap order.** Each version is self-sufficient; hardware is assigned to a version on purpose. Don't pull a later version's concern into an earlier one.

## Versioning (the Lumi standard)

Roadmap phases are `vA.B` (`A` = version v0–v6, `B` = phase), released as **`A.B.C`** and tagged **`vA.B.C`**, where `C` is a post-release fix on that phase. No zero padding. Releases are cut per phase. **Never bump a version without explicit user confirmation.**

Issue ids are **`RF-###`**, globally sequential across every phase and every regeneration run — never reset. `generate-issues` resolves the next id as `max(highest on GitHub, highest in specification/roadmap/implementation/*-issues.md) + 1`.

## The generation pipeline (`.claude/skills/`)

Imported from the `agent-arena-sandbox` project and retargeted to RoboFace (`RF-###`, `specification/roadmap/implementation/`, `vA.B` phases, Gemini-only, this repo's seams). Two workflows; do not mix them within one version.

**A. GitHub-driven — the one that runs from the current state:**
`generate-issues` → `upload-issues` → `execute-issues` → `review-and-fix-issues` → `release-version`, orchestrated per phase by **`/ship-phase <selector>[,…] [--no-harden]`**, with a `harden-findings` sweep at each phase boundary. It generates the issues files as its first step, so it needs nothing pre-populated. Requires an authenticated `gh`.

**B. File-driven, offline:**
`reconcile-issues vA.B` → `execute-issues-file vA.B` → `review-and-fix-issues vA.B` → `release-version A.B.0`, orchestrated by **`/ship-solution`**. It executes from `specification/roadmap/implementation/vA.B-issues.md` and **cannot generate one** — with none present it stops and names the versions it would need. Use workflow A first, or author the files.

`/reset-generated` clears what a tracked run created, reading the run's own event log. It never touches `codegen/`, `.claude/`, `specification/` (outside `roadmap/implementation/`), or `.env*`.

Rules the skills depend on: **one issue = one commit**; respect each file's dependency tree; tests ship with the feature; a seam change carries its ARCHITECTURE update and contract test; ask or reconcile rather than guessing when an issues file disagrees with the code.

## Code-generation tracking (`codegen/`)

Imported alongside the skills: an append-only event log per run, Claude Code hooks that observe tool use independently of what the skills claim, a reducer, and a live dashboard. The skills emit into it (`python3 -m tracker.emit …`, run from `codegen/`); the hook is registered in `.claude/settings.json`, which needs a session restart to take effect.

```bash
python3 -m venv .venv && .venv/bin/pip install -r codegen/requirements.txt   # once
cd codegen && ../.venv/bin/pytest         # 542 passing, 17 skipped
cd codegen && ../.venv/bin/python -m uvicorn dashboard.server:app --port 8420
```

Port 8420, never 8000 — the RoboFace server will own 8000. Run its pytest **from inside `codegen/`**: from the repo root it would also collect the server's suite. `codegen/runs/` and `codegen/var/` are gitignored — a log is machine-local evidence. Its design documents still narrate the run they were derived from in another project; the machinery is what applies here. See [codegen/README.md](codegen/README.md).

## `pyramid` — the reference project

`~/development/pyramid` (AtomS3R + Echo Base) is RoboFace's conceptual model — **separate project, zero shared code**. Worth reading before designing a subsystem: its `server/pyramid_server/` decomposition (pure `protocol.py`, router, orchestrator + sentence buffering, `providers/base.py` Protocol seams with mocks), `specification/EMOTION_FACE.md` (the origin of `EmotionFrame`: layer model, recipe table, `IFaceRenderer`, crossfade/ttl, asset manifest, lip-sync viseme thresholds), and `docs/firmware/FIRMWARE_ARCHITECTURE.md` (the pure/glue split and the `platformio.ini` shape). Its "mind" roadmap — role console, MCP, long-term memory, temperament — is deliberately **not** carried over.

`~/development/lumi` is the source of this repo's documentation shape and versioning standard.

## Toolchain (once the product directories exist)

- **Server:** Python, FastAPI + websockets, SQLite from v4; `pytest`, `ruff check server tests`, `mypy server` (strict).
- **Firmware:** C++ / PlatformIO for the Core S3 — M5Unified + M5GFX sprites, PSRAM for audio buffers, the face framebuffer and JPEG frames; esp-sr for the AFE in v3.4. `pio run -e cores3`, `pio test -e native`, `pio device monitor`.
- Add the concrete commands to this file when the first server or firmware directory lands.
