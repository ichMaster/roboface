# Roadmap — RoboFace

Seven self-contained versions, built in order: **v0** Skeleton (server + device + the wire contract) → **v1** Voice (the full loop, hands-free) → **v2** The living face (procedural face, emotion channel, lip-sync, touch/motion, skins) → **v3** Vision (look-and-tell, presence, the background emotion channel, AEC) → **v4** The simple mind (canon, memory, one tool, world context) → **v5** Bottom3, optional (the emotion halo) → **v6** FIRE compatibility (a second board behind capability flags).

Versions are numbered from 0; phases inside a version are numbered `vA.B` (A = version, B = phase), e.g. `v2.3`. Each phase lists a **Goal**, a short description, a **Tasks** list, a **Definition of Done (DoD)** and the **Tests** that encode it (see [ARCHITECTURE.md](ARCHITECTURE.md) §Testing and CI). Phases that draw on the screen or handle input implement [features/DEVICE_UI.md](features/DEVICE_UI.md).

**Versioning (`A.B.C`).** `A` = roadmap version (v0→0 … v6→6), `B` = phase within it (`v2.3` → `2.3.0`), `C` = a post-release fix on that phase. Roadmap phase `vA.B` → release `A.B.0`, tagged `vA.B.0`; a fix after it bumps `C`. Releases are cut per phase. **Never bump the version without explicit confirmation.**

Arc of the two axes: the **device** grows screen → speaker → microphone → touch/IMU/proximity → camera → halo/second board; the **server** grows protocol → orchestrator → emotion engine → vision → mind. The device stays thin throughout — every capability added to it is I/O, never a decision. Complexity is added only by version, never all at once.

**The device UI is specified once and referenced from the phases that build it:** [features/DEVICE_UI.md](features/DEVICE_UI.md) — indicators, the input map for both boards, the four notification ranks and the six full-screen states — with [device-ui-prototype.html](device-ui-prototype.html) as its interactive mock-up.

**Streaming is built in from the first stage that has one, never retrofitted.** v0.2 streams the model's token deltas; v1.1 splits those deltas into phrases and streams synthesis and playback so speech starts before the reply exists; v1.2 streams the microphone while you are still speaking; v1.3 streams recognition over a WebSocket so the transcript is ready the moment you stop, and pins the per-stage budgets; v3.4 makes the whole thing interruptible. Each phase's DoD asserts the streaming property (interleaved frames, not two bursts), because a pipeline that only streams "later" never does. See [ARCHITECTURE.md](ARCHITECTURE.md) §Turn lifecycle.

---

## v0 — Skeleton

The two ends meet. A new Python server (written from scratch, with Pyramid as the conceptual model only) and a Core S3 that is a WSS client: a text turn goes device → server → Gemini → device, and the screen shows the turn state as a face rather than a label. No audio, no camera, no sensors. This version establishes everything later versions extend: the pure protocol module, the provider seams, the fake device, and the contract tests. Depends on: nothing.

### v0.1 — Server skeleton and the wire contract

**Goal:** a server exists, a fake device can talk to it, and the wire format is pinned.

Stand up `server/` (FastAPI + websockets) with the **pure `protocol` module** as its centre: message-type constants, the enumerated `error.code` set, the JSON frame codec, `hello` proto-version negotiation, and the rule that binary frames carry raw payloads with no envelope. Add the `router` — one connection state machine per device — and the **fake device** the whole test suite will use. No model call yet: `text_in` echoes through a mock provider.

**Tasks:**
- Scaffold the repo: `pyproject.toml` (ruff, pytest, `pythonpath=["server"]`), `server/.env.example`, `.github/workflows/ci.yml` running ruff + pytest.
- `protocol.py`: message types both ways, `ErrorCode`, the text-frame codec, `hello{device_id, proto_ver, audio_fmt, caps}` negotiation, PCM16/16 kHz/mono constants.
- `router.py`: accept a WSS connection, negotiate `hello`, hold per-connection state, dispatch frames, `ping`/`pong`, clean teardown in a `finally` block.
- A **fake device** test harness that speaks the protocol (connect, hello, `text_in`, receive).
- Structured logging keyed by `device_id` + `session_id`.

**DoD:** the fake device connects, negotiates `hello`, sends `text_in` and receives a `reply`; an unsupported `proto_ver` is rejected with `proto_unsupported`; ruff and pytest are green in CI.

**Tests:** contract — the full inbound/outbound message set, the error-code enum, and `hello` negotiation (this file changes only when the contract does); unit — codec round-trip, malformed frame rejection; integration — a `text_in` turn against a mock provider.

### v0.2 — Gemini and the orchestrator

**Goal:** a real answer from the model, streamed.

Add the **`LLMProvider` seam** and its single real implementation: **Gemini 2.5 Flash with `thinkingBudget: 0`**. Add the `orchestrator` that runs a turn and streams it — token deltas out as `reply` frames — plus the minimal system-prompt assembly and the error mapping from provider failures to `error.code`. The mock provider stays the default in tests.

**Tasks:**
- `providers/base.py`: `LLMProvider` as an async-iterator Protocol (`stream(system, messages) -> AsyncIterator[str]`), plus `ProviderError` carrying an optional `error.code` hint.
- `providers/gemini.py`: the Gemini 2.5 Flash adapter — streaming, `thinkingBudget: 0`, safety settings, key from `GEMINI_API_KEY`.
- `providers/mock.py`: canned deterministic streams (the default everywhere in tests).
- `orchestrator.py`: run a text turn and **stream it** — each token delta leaves as its own `reply{text, final}` frame the moment it arrives; the reply is **never accumulated and sent whole**. A separate **first-token budget** (~8 s) is awaited on `__anext__()` before the loop, so a stalled model fails fast while a slow-but-flowing one is not cut off mid-reply.
- Failure and abort semantics: map provider failures to `llm_timeout` / `llm_failed` / `rate_limited`; an aborted turn rolls the user message back out of history so the next turn starts consistent.
- `prompt.py`: assemble a minimal system prompt (a placeholder character until v4.1).

**DoD:** a `text_in` from the fake device returns a real Gemini answer **arriving as a stream of deltas, first delta well before the reply is complete**, with the first-token budget enforced; every provider failure surfaces as an enumerated `error` and leaves history consistent; no test makes a paid call.

**Tests:** contract — the `LLMProvider` interface and the `reply` delta shape (a delta stream, not one final message); unit — prompt assembly, error mapping, the first-token timeout, history rollback on abort; integration — a full `text_in → reply` turn against the mock asserting **multiple** `reply` frames.

### v0.3 — Firmware skeleton: Core S3 as a WSS client

**Goal:** the board talks to the server.

Create `firmware/` as a PlatformIO project for the Core S3 with the two-layer split from the start: Arduino-free pure logic (framing, the turn state machine, backoff) and glue (`net`, `ws`, `state`, `ui`). USB serial is the debug channel and the `text_in` source — the same trick Pyramid used to get a testable loop before audio existed.

**Tasks:**
- `platformio.ini`: an ESP32-S3 profile with M5Unified + ArduinoJson + a WebSockets client, PSRAM enabled, plus a `native` env for host tests; `config.example.h` → gitignored `config.h` (WiFi + server URL).
- `net`: WiFi bring-up and a reconnect supervisor with exponential backoff + jitter.
- `ws`: the WSS client — hello with `caps`, JSON frames, binary frames, ping/pong, reconnect.
- Pure logic: the device state machine (`boot → wifi_connecting → idle → listening → thinking → replying`, plus `offline`/`error`), line reader, backoff — all host-testable.
- Serial debug channel: a typed line becomes `text_in`; replies print to serial.

**DoD:** on the real board, typing a line over serial produces the model's reply over serial; pulling WiFi moves the device to `offline` and it reconnects on its own; `pio test -e native` passes and `pio run -e cores3` compiles clean.

**Tests:** host (`pio test -e native`) — state transitions, frame framing/parsing, backoff; server-side integration unchanged; manual DoD check on hardware.

### v0.4 — Face stub and the renderer interface

**Goal:** the screen shows a face, not a status string — and the interface that every later tier implements exists.

Introduce `IFaceRenderer` and a **stub renderer**: a static face per device state, drawn with M5GFX primitives on the 320×240 screen. This is the smallest thing that proves "state is expressed as a face", and it fixes the interface (`begin`/`show`/`setAudioLevel`/`tick`) before the real renderer arrives in v2.

**Tasks:**
- `IFaceRenderer` in pure/glue split form; a `StubRenderer` mapping each device state to a simple drawn face.
- Basic M5GFX sprite plumbing: a full-screen sprite in PSRAM, DMA push, no tearing.
- Wire the state machine to `show()`; keep a tiny corner debug line (toggleable) for diagnosis.
- The first **chrome**, per [features/DEVICE_UI.md](features/DEVICE_UI.md): the top-band status cluster (link arcs, battery pill) with the settle-and-fade rule, and the fault toast carrying the enumerated `error.code`. Chrome lives in the outer 28 px band and never overlaps the face.
- Screen brightness from config; the screen never shows a state as a word — a state is the face, a fault is the error face plus its code.

**DoD:** the board shows a distinct face for `idle`, `listening`, `thinking`, `replying`, `offline` and `error`; link and battery appear when they matter and fade when they settle; a fault shows the error face plus its code; the renderer interface is the only way the app draws a face; v0 is a complete, self-contained skeleton.

**Tests:** host — the state → face-recipe mapping is total over the state enum, and the chrome visibility rules (fade after settle; a fault never auto-dismisses); manual DoD check on hardware (each state visibly distinct, no tearing, chrome clear of the face area).

---

### v0.5 — Serial chat console

**Goal:** hold a text conversation with the board as the display, before any audio exists — and prove the Ukrainian round trip renders on the panel.

v0.4 closed the skeleton: a turn goes device → server → Gemini → device and the screen shows the turn *state* as a face. What it cannot show is the turn's **content**, so the only way to read a reply is over the serial cable. This phase adds a console mode that borrows the screen to show the conversation, the way `/faces` already borrows it for the self-test — an instrument, explicitly entered and explicitly left.

**This does not relax the face rule.** MISSION's "a device state is a face, never a word" is about *state*: `thinking` must never appear as the word "thinking". A transcript is content, not a state label, and the mode announces itself rather than pretending to be the product. Outside it, nothing changes.

**Ukrainian must render.** The product's language is Ukrainian ([MISSION](MISSION.md)) and the system prompt answers in it, so a console that can only show Latin text would be able to display the questions and not the answers. M5GFX bundles no Cyrillic font — its `efont` set is JA/CN/KR/TW and the built-in `Font0` is ASCII — but it accepts **U8g2 font arrays** through `lgfx::U8g2font`, which is the same mechanism its own bundled fonts use, and it already decodes UTF-8 when drawing. So the fix is an embedded font, not a text pipeline.

**Tasks:**
- `/chat-on` and `/chat-off` serial commands, also drivable from `tools/board.py --send`. `/chat-on` prints a prompt on serial and takes the screen; `/chat-off` restores the face **and the device state that was showing before**, the way the `/faces` self-test restores its saved state.
- The transcript view: the message just sent, and the reply as its deltas arrive. Drawn into the existing full-screen sprite and pushed with it, so it cannot tear against chrome.
- A Cyrillic-capable U8g2 bitmap font, embedded as an array and wrapped in `lgfx::U8g2font`. It must be licence-compatible with this repository (MIT) — a GPL-licensed font such as Unifont is not, whatever its coverage.
- **Pure:** UTF-8-safe word wrapping and a bounded transcript buffer, in `pure/`, host-tested. Wrapping counts codepoints rather than bytes; a Ukrainian reply is two bytes per letter, so a byte-based wrap would both mis-measure every line and split a character in half.
- **Glue:** the drawing, the font, and the command handling.
- Chrome keeps its band and its rules — link and battery are facts, and a console that hid them would hide a dropped link at the moment it matters most.
- The mode survives a WS reconnect and does not interfere with the existing "reply outside a turn" guard.

**DoD:** `/chat-on` prints a prompt on serial and replaces the face with an empty transcript; a line typed at `tools/board.py` appears on the screen as the outgoing message and its reply appears there as it streams; **a Ukrainian reply is legible on the panel** — no boxes, no mojibake, no half-characters at a line break; a reply longer than the screen scrolls or truncates deliberately rather than overrunning the face area; `/chat-off` restores exactly the face and state that were showing before; chrome behaves as it does outside the mode.

**Tests:** host — wrapping is total over the awkward inputs (empty, whitespace only, a single word longer than a line, mixed Latin/Cyrillic) and **never splits a UTF-8 sequence**; the transcript keeps its last N lines and cannot grow without bound however long the reply; the mode toggle restores the saved state. Manual DoD check on hardware, including a Ukrainian round trip read off the panel rather than off the serial log.

---

## v1 — Voice

The complete voice loop through the server, hands-free by the end. Output is built before input (validate the easy path first): TTS playback, then capture, then ASR, then VAD-driven active listening. At the end of v1 you can hold a spoken conversation with a face that still only shows states — the face itself comes in v2. Depends on: v0.

### v1.1 — Streaming TTS playback

**Goal:** typed text comes back as a voice.

Add the **`TTSProvider` seam** and the ElevenLabs **stream** endpoint (`pcm_16000` — straight into the device's playback format, no decode), the **phrase splitter** that hands completed clauses to TTS while the model is still generating, and device-side streaming playback. This is the phase where the pipeline stops being request/response: **the mouth opens before the sentence exists**.

**Tasks:**
- `providers/base.py`: `TTSProvider.synthesize(text) -> AsyncIterator[bytes]` (PCM16 16 kHz mono) + a mock that yields several chunks.
- `providers/elevenlabs.py`: the streaming endpoint (`/stream`, `output_format=pcm_16000`), voice id and model id from config, chunks yielded as they generate, `tts_failed` mapping.
- `sentence.py` — the **`PhraseSplitter`** (pure, no I/O): consume LLM deltas, emit a phrase at each sentence (`. ! ? …`) or clause (`;`) boundary **only when followed by whitespace** (so `3.5` and a boundary sitting at the end of the buffer don't split early), skip Ukrainian abbreviations (`напр.`, `т.д.`, `вул.` …), force-flush a runaway clause at `max_chars` on a word boundary, and `flush()` the tail for short replies.
- Orchestrator: for each completed phrase, synthesize and forward chunks immediately, with a **first-audio budget** (~3 s) on the phrase's first chunk; a failure mid-phrase aborts the turn with a mapped error.
- Firmware `audio_io` — **early/streaming playback**: switch the shared bus mic→speaker once on the first chunk, play each `tts_audio` frame as it arrives, drain and switch back on `tts_end`. The buffer is **lossless** — consume exactly what plays; never truncate in a callback.
- Underrun policy: a starved buffer waits rather than emitting silence into the middle of speech; volume from config; playback cleanly interruptible by `restart`/error.

**DoD:** a line typed over serial is answered **out loud**, and **speech starts before the model has finished generating** (verified by log timestamps: first `tts_audio` precedes the final `reply` delta); the first spoken phrase is never a half-word; an interrupted or failed synthesis ends the turn cleanly and returns to `idle`.

**Tests:** contract — `TTSProvider` and the `tts_audio`/`tts_end` framing; unit — the `PhraseSplitter` against a delta-by-delta fixture set (boundaries, decimals, abbreviations, runaway clause, tail flush) and the ring buffer's underrun/lossless behaviour; integration — `text_in → tts_end` against mocks with the fake device asserting **interleaved** `reply` and `tts_audio` frames, not two separate bursts.

### v1.2 — Microphone capture and push-to-talk

**Goal:** the device can send what it hears.

ES7210 capture at 16 kHz PCM16, one channel for now, **streamed chunk by chunk** as binary `audio` frames between `listen_start` and `listen_stop` — never buffered whole and uploaded at the end, because v1.3's streaming recognition depends on audio arriving during speech. Touch-and-hold on the face is the trigger — the backup PTT that survives all later versions.

**Tasks:**
- Firmware `audio_io` capture path: I2S in, 16 kHz mono PCM16, a small PSRAM chunk buffer sent as soon as it fills (target ~20–40 ms per frame), mic/speaker mode switching.
- Touch (FT6336U): press-and-hold starts `listen_start` + streaming from ~120 ms, release sends `listen_stop`.
- The **input-level meter** in the bottom band while listening (DEVICE_UI.md §Indicators) — the only "listening" signal; the word is never printed.
- Server `router`: accept binary `audio` frames only in the `listening` state; enforce a maximum utterance length.
- Loopback diagnostic: capture N seconds and play it back locally (a hardware smoke check, not a shipped feature).

**DoD:** holding the face streams speech to the server **while you are still speaking** (the server logs frames arriving before `listen_stop`, not after); releasing ends the utterance; oversize utterances are cut with a clean error.

**Tests:** host — the capture buffer math and the PTT state transitions; contract — binary `audio` frames only valid inside `listen_start`/`listen_stop`; integration — the fake device streams a canned PCM file and the server assembles it intact.

### v1.3 — ASR and the full voice loop

**Goal:** speak, and be answered by voice.

Add the **`ASRProvider` seam** with Deepgram **over a WebSocket, not batch REST** — audio is recognised **during** speech, so at the endpointing decision the transcript is already in hand and the ASR stage collapses to nearly zero. (Batch recognition pays ~1.4 s *after* the utterance ends; that single choice is most of the latency budget.) With this phase all three stages stream and the loop closes.

**Tasks:**
- `providers/base.py`: `ASRProvider.stream() -> AsyncIterator[ASRChunk{text, is_final}]` fed by pushed audio frames, + a mock that replays scripted events.
- `providers/deepgram.py`: the `wss://` listen URL — `linear16`, 16 kHz, mono, Ukrainian, `interim_results=true`, `smart_format=true`, **server-side `endpointing`** (~500 ms) and an `utterance_end_ms` (~1000 ms) backstop for when endpointing never fires. The connector is injectable so tests drive a fake socket.
- **Phrase-hold** in the utterance tracker: an un-punctuated `speech_final` (a breathing pause mid-sentence) is **held** for the continuation or the `UtteranceEnd` backstop — the character must never answer half a phrase.
- Interims out as `asr_partial`, the assembled utterance as `asr`; the LLM stage starts the moment the final lands.
- Orchestrator: the full streaming pipeline with **per-stage budgets** (ASR ≤ ~5 s, LLM first token ≤ ~8 s, TTS first audio ≤ ~3 s) and cancellation on device disconnect; a stage breach aborts to its enumerated error and returns the device to `idle`.
- Firmware: render `asr_partial`/`asr`/`reply` to the serial debug channel; the screen still shows states.
- Latency instrumentation: log `listen_stop → asr final`, `→ first reply delta`, `→ first tts_audio`, each as its own number.

**DoD:** speaking Ukrainian yields a spoken answer, with the transcript already resolved when the utterance ends; **first audio < ~1.5 s after `listen_stop`** on the development network, and the logged breakdown shows the ASR leg as the *smallest* of the three, not the largest; a breathing pause mid-sentence does not trigger an early answer; each stage's failure maps to its own error code.

**Tests:** contract — `ASRProvider` and the `asr_partial`/`asr` frames; unit — the utterance tracker against scripted Deepgram events (interims, punctuated final, un-punctuated final held then continued, `UtteranceEnd` backstop), the URL builder, and each stage budget; integration — `audio → tts_end` end to end against mocks with **interleaved** stage frames, plus the three failure paths (ASR, LLM, TTS).

### v1.4 — Active listening (VAD)

**Goal:** hands-free — no button at all.

Make **active listening the default**: the mic is always open, and an on-device VAD decides when to *open the uplink stream* — speech start opens it, and the recogniser's endpointing (v1.3) closes the utterance, with the local end-pause as the backstop when the link or the recogniser is unavailable. Half-duplex is enforced (listening pauses while the device speaks) until AEC arrives in v3.4; touch-and-hold remains as the backup PTT.

**Tasks:**
- Pure-logic VAD endpointer: energy/zero-crossing based, with a configurable pre-roll, minimum-speech and end-pause; entirely host-testable.
- Wire it to the turn state machine: speech start → `listen_start` and the audio stream opens immediately (pre-roll included so the first syllable is never lost); the utterance ends on the server's endpointing decision, or on the local end-pause as the backstop.
- Half-duplex guard: capture is suspended for the duration of playback and resumes after `tts_end`.
- Config: sensitivity, end-pause, and an on/off switch (falling back to PTT-only).
- False-trigger handling: an utterance below the minimum length is discarded without a turn.

**DoD:** speaking into the room starts and ends a turn with no touch; the device does not trigger on its own speech; the backup PTT still works; v1 is a complete hands-free voice companion.

**Tests:** host — the endpointer against recorded PCM fixtures (speech, silence, short noise, long pause), deterministic with an injected clock; integration — a fake device driving VAD-marked utterances through a full turn.

---

## v2 — The living face

The face becomes the product. A procedural Stack-chan-style renderer with an idle loop, the server-side emotion engine emitting `EmotionFrame`, lip-sync from the playback envelope, the two-level touch/motion/proximity interaction model, dual-mic gaze — and finally the four spirit skins, switchable over the same contract. Depends on: v1.

### v2.1 — Procedural face and the idle loop

**Goal:** the face is alive even in silence.

Replace the stub with a **layered procedural renderer**: a small layer bank (background/glow, base, eyes, brows, mouth, overlay), an emotion recipe over it, crossfade between expressions, and the idle loop — blinking every few seconds, a slow "breathing" bob and micro gaze drift. Driven for now by device state; the server channel arrives in v2.2.

**Tasks:**
- The layer bank and the emotion recipe table as **pure logic** (host-testable), separate from drawing.
- `ProceduralRenderer`: M5GFX sprites per layer, composited with DMA push at a steady frame rate.
- The idle loop in `tick()`: randomized blink, breathe, gaze micro-drift, all scaled by `intensity`.
- Crossfade (~150–250 ms) between recipes; `ttl_ms` relaxation toward `neutral`.
- Frame budget: hold ~15–20 FPS with the audio pipeline running.

**DoD:** on an idle device the face blinks, breathes and drifts its gaze; switching state crossfades rather than snapping; the recipe table covers the full emotion enum; no tearing and no audio glitches at target FPS.

**Tests:** host — the recipe table is total over the enum; the idle scheduler is deterministic under an injected clock; crossfade interpolation math; manual DoD check on hardware for FPS and tearing.

### v2.2 — The emotion channel

**Goal:** the server decides how the face feels.

Add the server-side **emotion engine** and the `EmotionFrame` contract end to end: per turn and per state change the server emits a frame, the device renders it, and turn states stop being an on-device decision. The model reports its own state alongside the reply; the server validates it against the enum and falls back safely.

**Tasks:**
- Structured output from Gemini: `{reply, emotion, intensity}`; validation (unknown emotion → `neutral`, clamp intensity), never trusting raw output.
- `emotion.py`: map turn state + the model's report to an `EmotionFrame` (including `gaze`, `accent_color`, `speaking`, `ttl_ms`).
- Protocol: the `emotion` frame, emitted per turn and on state change, including outside a turn.
- Firmware: `show(EmotionFrame)`, `ttl_ms` relaxation, `speaking` gating; the device no longer picks emotions.
- Error paths: `error` emotion for enumerated failures.

**DoD:** the face's expression comes from the server for every turn and state change; a malformed or unknown model emotion never reaches the screen; with the server unreachable the device relaxes to `neutral` and keeps its idle loop.

**Tests:** contract — the `EmotionFrame` schema and the emotion enum (the file that must change with the contract); unit — validation/fallback, state → frame mapping; integration — a turn emits the expected frame sequence to the fake device.

### v2.3 — Lip-sync

**Goal:** the mouth moves with the voice, with no help from the server.

Compute a short-window RMS envelope of the audio being played and map it to mouth openness over four bands, at ~15–20 FPS, layered over whatever expression is current. `speaking` from the `EmotionFrame` only gates it.

**Tasks:**
- Pure-logic envelope: windowed RMS with smoothing and the four-band viseme mapping (closed/small/mid/wide).
- Feed it from the playback ring buffer (`setAudioLevel`) without stalling I2S.
- Layer the mouth viseme over the expression mouth; restore the expression mouth when `speaking` is false.
- Tune smoothing so the mouth reads as speech rather than as noise.

**DoD:** while the device speaks, the mouth animates in time with the audio; when it stops, the expression's own mouth returns; the server sends nothing per frame for any of this.

**Tests:** host — the envelope + band mapping against PCM fixtures (silence, speech, clipping), including smoothing behaviour; manual DoD check for perceived sync.

### v2.4 — Touch, motion and proximity — the two-level model

**Goal:** the robot reacts to being touched and moved — instantly, and then in character.

Implement both levels: **local reflexes** in under 100 ms (never interrupting speech or listening) and the `event{}` report that lets the character answer with an emotion change or a line. Proximity wakes the face and pulls its gaze.

**Tasks:**
- Touch zones over the face (cheek/forehead/eyes) and gesture recognition per [features/DEVICE_UI.md](features/DEVICE_UI.md) §Input: tap, multi-tap, stroke, poke, hold.
- IMU (BMI270) gestures: tilt, shake, picked up, upside down, free fall — all pure logic over the raw stream.
- Proximity (LTR-553): approach wakes the face and biases `gaze`; leave relaxes it.
- The reflex layer: a short animation composited over the current expression, bounded and non-blocking.
- `event{type, kind, meta}` to the server; server-side handling that may answer with an `emotion` frame and/or a spoken line.
- The hold is layered, not split (DEVICE_UI.md §Input): PTT from ~120 ms, converting to the v2.6 carousel only past 1.2 s without speech. **Affection** gestures emit `event{}`; **control** gestures (carousel, sheet, mute) are local UI and are deliberately not reported.

**DoD:** a tap, a stroke, a tilt, a shake and an approaching hand each produce a visible local reaction within ~100 ms without disturbing an ongoing turn; the same inputs reach the server and can produce a spoken reaction; with the server offline, every reflex still works.

**Tests:** host — gesture classification from recorded touch/IMU fixtures; the reflex layer never cancels a turn; contract — the `event{}` shape and its `kind` enums; integration — an event triggers the server's reaction path.

### v2.5 — Dual microphones: direction and a cleaner signal

**Goal:** the face looks at whoever is speaking, and hears them better.

Use both ES7210 channels: a rough left/right direction from the inter-mic time/level difference drives `gaze`, while channel selection/summing improves the signal sent to ASR. The uplink stays mono — the stereo work is done on the device.

**Tasks:**
- Pure-logic direction estimate (cross-correlation or level difference) with smoothing and a confidence threshold.
- Feed the estimate into the local gaze target, and report it to the server so `EmotionFrame.gaze` can carry it.
- Channel selection/summing before uplink; the server still receives mono 16 kHz.
- Start of dual-channel VAD robustness: prefer directed speech over diffuse noise.

**DoD:** speaking from the left or right visibly turns the face's gaze that way within a moment; ASR accuracy in a noisy room is no worse than the single-channel baseline; the uplink byte rate is unchanged.

**Tests:** host — direction estimation on synthetic and recorded two-channel fixtures (left/right/centre/noise), smoothing and threshold behaviour; integration — the gaze value round-trips through `EmotionFrame`.

### v2.6 — Spirit skins and the carousel

**Goal:** five faces, one contract.

Add the four sprite skins — **ghost, flame, jellyfish, cloud** — over the same `EmotionFrame`, each expressing emotion additionally through its "element" (flame colour, cloud weather, jellyfish glow, ghost blush and tear). Switching is `config_updated{face_set}` from the server or a long press on the device.

**Tasks:**
- The **skin manifest** schema: layer keys, palette, per-emotion recipe overrides, element behaviour; one directory per skin under `assets/`.
- Load a pack into PSRAM at `begin()`; fall back to the procedural face when a pack is missing or invalid.
- The four packs authored against the shared expression grammar (`specification/face-prototype.html` is the reference).
- `config_updated{face_set}` handling server-side and on the device; the **carousel UI** — a hold that passes 1.2 s without speech converts to the dot strip, slide to choose, release to confirm, whisper toast on change (DEVICE_UI.md §Input); the active skin reported in the next `hello`.
- Bottom-band arbitration: carousel > toast > level meter, one tenant at a time.

**DoD:** all five faces (Stack-chan + four spirits) render the same `EmotionFrame` correctly and are switchable both ways; adding a skin requires no renderer code change; a corrupt or missing pack degrades to the procedural face instead of failing; v2 is the complete living face.

**Tests:** contract — the skin manifest schema and the `config_updated{face_set}` frame; host — manifest loading, validation and fallback, and that each pack's layer keys are total over the emotion enum; manual DoD check of the four skins on hardware.

---

## v3 — Vision

The camera opens. **Most of what it sees is understood on the server, locally, without a model call** — faces, where they are, and what the face is doing. Only the one question that needs language ("what do you see?") goes to Gemini. The version ends with the hardest audio work in the project: the esp-sr front end, which both makes listening full-duplex and gates the uplink on a name — and closes by learning who it is talking to, by voice and then by face. Depends on: v2.

**Why the split is by frequency rather than by capability.** A cloud call is 500–2000 ms; a head turns in 200. Tracking a face through a model call is not slow, it is impossible — and an emotional read that costs a paid call every few seconds is one nobody leaves switched on. Local vision is continuous, free and private; the LLM is occasional and rich. Each does what the other cannot.

**The privacy story is stronger than v2's was, and for a structural reason.** Frames from the continuous channel **never leave the LAN** — they are processed on the server beside the device and discarded. Only the explicit "what do you see?" frame is sent to Google, on a person's direct request, once.

### v3.1 — Look and tell

**Goal:** "what do you see?" is answered.

Capture a JPEG from the GC0308 on demand, announce it with `image_in` and send it as a binary frame; the server attaches it to that turn's Gemini call as multimodal input and answers by voice through the normal pipeline. **The one place the LLM is the right tool**: describing a scene in language is what it is for, and no local classifier substitutes — a list of labels is not a description.

**Tasks:**
- Firmware camera bring-up: init, capture, JPEG encode into PSRAM at a modest resolution, memory bounded.
- Protocol: `image_in{reason, w, h}` followed by the binary JPEG; size limits and rejection rules.
- Server: attach the image to the LLM call as multimodal input; map failures to `vision_failed`.
- Trigger: an explicit request in the turn (recognised server-side) asks the device for a frame.
- Privacy: the frame lives in memory only and is dropped as soon as the turn ends — and the **lens indicator is lit for as long as the camera is powered**, with no fade timer and no way to hide it (DEVICE_UI.md §Indicators).

**DoD:** asking what it sees produces a spoken description of what is actually in front of the camera; no frame is ever written to disk; an oversized or failed capture ends the turn with `vision_failed` and a visible error face.

**Tests:** contract — the `image_in` frame and the binary JPEG rule; unit — size/dimension guards, the multimodal call shape against a mock; integration — `image_in → reply` end to end with a fixture image.

### v3.2 — The vision seam: faces, presence and gaze

**Goal:** the face looks at the person, and knows when one arrives.

A **fourth provider**, `VisionProvider`, alongside `LLMProvider`, `ASRProvider` and `TTSProvider` — with a mock, so the suite needs neither a camera nor a model. It joins the existing provider-seam category rather than adding a sixth seam: the five pinned seams are categories, and this is a new member of one of them. Its real implementation runs face detection on the server (MediaPipe on CPU, single-digit milliseconds on the N100 the server is) over a low-rate stream of frames the device sends while presence mode is on.

**Face detection replaces the frame-difference presence detector** the previous plan called for. It is the same computation, strictly better evidence, and it removes a subsystem rather than adding one: "a face is present" is a stronger claim than "some pixels changed", and it comes with a position.

**The position rides the seam that already exists.** `EmotionFrame.gaze` has carried the voice direction since v2.5 and its authority order is documented; a face is simply a better producer for it. No new level is needed — the round trip is a few milliseconds on a LAN, which is far inside the time a head takes to move.

**Tasks:**
- `VisionProvider` seam + mock; a real adapter behind it. The seam takes a JPEG and returns faces with positions and confidence — **no emotion here** (that is v3.3, and mixing them would make one hard to replace without the other).
- Device: low-rate frame capture in presence mode. **The rate is the design decision**: continuous tracking does not need thirty frames a second, and the socket that would carry them is the one that blocked for 142 ms in v2.6.2 while a reply played. Start at 3–5 fps, interpolate the gaze between reads, and drop frames rather than queue them when the link is busy.
- Presence from faces: arrival, departure, hysteresis and a cooldown, in pure logic against fixtures.
- Gaze: the largest/nearest face's position becomes `EmotionFrame.gaze`; the existing authority order settles the rest.
- Server-side greeting policy: at most one greeting per arrival, quiet-hours aware, never during a turn.
- Config gate: presence mode is off by default and enabled explicitly. The **lens indicator is lit the whole time presence keeps the camera powered** — presence is not an exemption from the privacy rule.

**DoD:** a person moving in front of the device is followed by its eyes within a moment; sitting down in front of an idle device wakes the face and can produce a greeting; leaving and returning does not spam greetings; with presence off the camera stays dark outside explicit turns; **no frame from this channel reaches any external service**, asserted in a test rather than promised.

**Tests:** contract — the `VisionProvider` seam; host — presence hysteresis and cooldown against fixtures, gaze interpolation between reads under an injected clock; unit — the greeting policy; integration — a fixture face reaching the server moves `gaze` in the next frame, and the frame rate limiter drops rather than queues under a busy link.

### v3.3 — What the face is doing, in the prompt

**Goal:** the character responds to how you look, without guessing at a category.

Every turn carries a short description of the person's expression into the system prompt. It is measured locally by the same `VisionProvider` and costs nothing: no background task, no separate model call, no expiry, no rule about not blocking a turn — the read is already there when the turn starts.

**Facial muscle activity, not an emotion label.** MediaPipe's face landmarker outputs blendshape coefficients — `mouthSmile`, `browDown`, `eyeSquint`, `jawOpen` and forty-eight more. Those are measurements. A seven-class emotion classifier on top of them is a guess, and one built on a model of "basic emotions" that is scientifically contested and much less accurate in a real room than on a benchmark. So the seam reports **what the face is doing** — *"smiling, brows raised"* — and the LLM interprets it in the context of the conversation, which is the thing the LLM is actually good at.

A character confidently mirroring a wrong read is worse than one that does not try, which is why this phase produces a hint rather than an instruction.

**Tasks:**
- Extend `VisionProvider` with blendshape output; a threshold table turning coefficients into a small vocabulary of plain descriptions.
- A one-line note in the next turn's system prompt, and only there. Absent when no face is present or confidence is low — **absence rather than a neutral default**, the distinction v2.5 established for gaze.
- Optional mirroring on the face via an out-of-turn `EmotionFrame`, gated on confidence and off by default.
- The vocabulary is a closed, tested table: a coefficient with no description is a silent gap.

**DoD:** with presence on, the character's tone adapts to a visibly happy or unhappy person; the prompt line is absent when nobody is in front of the camera; **turn latency is unchanged**, measured with the channel on and off; disabling the channel changes nothing else.

**Tests:** host — the coefficient-to-description table, total over its vocabulary and with its thresholds; unit — the prompt line present exactly when a confident read exists and absent otherwise; integration — the note appears in the next prompt and only there, and the turn's frame sequence is unchanged.

### v3.4 — AEC, barge-in and the name

**Goal:** you can interrupt the character mid-sentence — and nothing leaves the device until it is called.

Integrate the Espressif audio front end (esp-sr) using the playback reference channel from the ES7210. Two halves of one package arrive together because they are one integration: the **AFE** (AEC + noise suppression + VAD) so the microphone no longer hears the speaker, and **WakeNet** so the uplink is gated on a name. Active listening becomes full-duplex and the half-duplex rule is retired.

**The largest phase in the project, and knowingly so.** Both halves live in the same audio front end and both must run *before* anything reaches the network, so splitting them would mean integrating one library twice.

**The wake word is a privacy and cost change before it is a convenience.** Today the endpointer fires on any speech loud enough — a television, a conversation between two other people in the room — and the audio goes to the server *and* onward to a paid recogniser. Measured on the board in a normal room: **436 speech starts against 4 completed utterances in three minutes.** A name closes that: the microphone still hears everything and the network hears nothing. It is also what would let active listening ship **on** by default, which it does not today and for exactly this reason.

**Tasks:**
- Bring esp-sr into the build; wire the playback reference channel into the AFE.
- Replace the v1.4 endpointer's front end with the AFE's VAD while keeping the same pure-logic turn semantics.
- Barge-in policy — **a pause and a queue, not a discard**: detected speech pauses playback and holds what is queued; the in-flight phrase is requeued whole so it can replay intact rather than resuming mid-word. Playback resumes when the interrupting utterance commits, or on an empty final / a nothing-to-flush `UtteranceEnd` (the noise case), with a watchdog (~4 s) so silence can never leave the character permanently muted. One barge-in per burst — the pause itself is the debounce.
- Server-side cancellation: an in-flight turn is cancelled cleanly on a committed barge-in, with **no orphaned `tts_audio` frames after cancellation** (an epoch/turn id on audio frames, so late chunks from the abandoned turn are dropped rather than played); a failed turn still resets the pipeline.
- Remove the half-duplex guard behind a capability flag (boards without AEC keep it).
- **WakeNet beside the AFE**, sharing the same front end. Gate the uplink on it: `beginListening` is refused unless the name was heard within a short window, or the person used one of the two deliberate paths.
- **Order the custom model before the phase, not during it** — it is procurement rather than code. WakeNet ships with `Alexa`, `Hi ESP`, `Hi Lexin` and several Chinese words; a name of our own must be produced by Espressif, either from a corpus (500+ speakers including 100 children, fifteen repetitions each at three speeds — a data-collection project) or from TTS samples, which is the realistic route and was improved in WakeNet10. Carry a built-in word as the interim so the gating can be finished and tested before the real model exists. Choose a name of 3–6 syllables: a single short word detects measurably worse than a name with a greeting attached.
- **The two deliberate paths stay and are not degraded.** Press-and-hold is push-to-talk from the first millisecond, and the microphone button is a control someone chose. Three ways to start a turn, each for a different situation: the name across a room, the hold when precision matters, the button when the room should not hear you say a name.
- The wake is a **local fact and is not reported** — the character has no business knowing it was called and then not spoken to. Only the utterance that follows goes on the wire.
- A false-wake counter in the status line, because the number that decides whether a wake word is usable is how often it fires when nobody spoke, and that number is only knowable from a room.

**DoD:** speaking while the device is talking interrupts it and starts a new turn; the device does not trigger on its own voice; on a board without AEC the half-duplex behaviour is unchanged; saying the name from across a room starts a turn; with the name armed and nobody calling it, **no audio frame leaves the device** over ten minutes in a normal room with speech in it — asserted from the server's own frame counters rather than from the device's word; press-and-hold and the microphone button behave exactly as before; a board without the custom model falls back to the interim word and says so at boot rather than silently never waking.

**Tests:** host — the barge-in decision logic against fixtures with playback bleed, the queue/requeue semantics (nothing queued is lost, the in-flight phrase replays whole) and the resume watchdog under an injected clock; the wake's arming window and its expiry under an injected clock, and that a refused `beginListening` leaves the endpointer **reset** (the v1.4 lesson: a refused window that leaves the endpointer convinced speech is ongoing makes the device deaf for the rest of the session); integration — a committed barge-in cancels the in-flight turn server-side and no `tts_audio` from the abandoned turn is played, and a fake device that never wakes sends no audio through a full session; manual DoD check on hardware, which is the only place a false-wake rate exists.

### v3.5 — Who is speaking

**Goal:** the character knows who it is talking to, and says the name.

**That is the whole of it in v3.** Recognise, and address the person by name. Nothing is stored about
them beyond the name they enrolled under, nothing is recalled, and the conversation is as
stateless as it was before. A person becomes a real user with a history in **v4.2**, and keeping
those apart is deliberate: identity is a measurement and memory is a policy, and the second is much
easier to get wrong.

A speaker-identification provider on the server, local like the rest of v3's continuous half: an ECAPA-TDNN embedding from the utterance the turn already carries, compared against enrolled voices by cosine distance. Roughly fifty milliseconds on the machine the server is, on audio that is already there — **no new data path and no new indicator**, which is most of why voice comes before face.

**A desk companion is talked to, not necessarily looked at.** Voice works in the dark, works with presence mode off and the camera unpowered, and answers the question that actually matters during a conversation: not who is in the room, but who is speaking now.

**"Unknown" is a first-class answer, not a nearest match.** The same rule this project applies to `Gaze | None` and `DirectionEstimate.present`: below the confidence threshold the identity is *absent*, and absent is not "probably Vitalii". A character that confidently calls someone by the wrong name is worse than one that does not use names.

**Tasks:**
- A `SpeakerIdProvider` alongside the other providers, with a mock; the real one is an ECAPA-TDNN embedding and a cosine comparison. Its output is `{person_id, confidence}` or nothing.
- **Explicit enrollment only.** A person is enrolled by being asked — three to five utterances — and never by the device quietly accumulating profiles of everyone who speaks near it.
- Storage: embeddings in the same SQLite the rest of the server uses, **never leaving the LAN**, listable and deletable by command. An embedding is a template for comparison and cannot be turned back into audio; say so in the docs, because half the worry about biometrics is about recordings.
- **An unenrolled speaker is "someone", not "unknown intruder".** No profiling of guests: no template is stored for a voice nobody asked to enroll.
- The identity reaches the prompt as **a name and nothing more** — *"you are speaking with Vitalii"*. No history, no facts, no per-person anything: that is v4.2, and this phase must not grow toward it.
- A threshold with hysteresis: an identity that flickers between two people mid-conversation is worse than one that stays wrong.

**DoD:** two enrolled people talking to the device in turn are **each addressed by their own name**; a third, unenrolled person is spoken to normally and without a name, rather than misidentified as either; clearing an enrollment really clears it; no embedding or audio leaves the LAN, asserted from the server's own outbound counters.

**Tests:** unit — the threshold and hysteresis against fixture embeddings, "unknown" below confidence, enrollment and deletion round-tripping through the store; contract — the `SpeakerIdProvider` seam and its mock; integration — two fixture voices across a session produce two distinct identities and an unenrolled third produces none.

### v3.6 — Who is here

**Goal:** the character greets the person who walked in, by name — before they have said a word.

Face recognition on top of the detection v3.2 already does: the same frames, one more step — an ArcFace embedding compared against enrolled faces. It answers the one question voice cannot, because it answers it **before anyone has spoken**.

**Everything from v3.5 applies unchanged** — explicit enrollment, templates that never leave the LAN, deletable, "unknown" as a real answer, no profiling of guests. This phase adds a second way to ask the same question, not a second policy.

**And one cost that voice does not have.** Face recognition needs the camera powered, which lights the lens indicator for as long as it runs. That is the privacy rule working as intended, and it is why this is a separate phase and separately switchable: someone may reasonably want to be recognised by voice and not watched.

**Tasks:**
- Extend `VisionProvider` with an embedding output; the comparison and store are shared with v3.5 rather than duplicated — one enrollment per person, two ways of recognising them.
- Greeting policy: uses v3.2's existing rules (one per arrival, quiet-hours aware, never during a turn) and adds the name when confident.
- **Voice wins a disagreement.** If the face says one person and the voice says another, the voice is talking. The face may be looking over a shoulder.
- Config: face recognition is separately switchable from presence, and off by default.

**DoD:** an enrolled person sitting down is greeted by name; an unenrolled one is greeted without a name rather than by the wrong one; with face recognition off, presence and gaze behave exactly as in v3.2; the lens indicator is lit whenever this is running.

**Tests:** host — the voice-over-face precedence and the confidence threshold against fixtures; unit — one enrollment record serving both providers; integration — a fixture face produces a named greeting and an unknown face produces an unnamed one.

---

## v4 — The simple mind

Everything here is server-side and deliberately small: an authored character, a memory that fits in a prompt, exactly one tool, and a block of world context. No console, no MCP, no temperament. Depends on: v0 (the pipeline); it can be built any time after v1 but lands here.

### v4.1 — Canon

**Goal:** RoboFace is a character, not a default assistant.

An authored canon file — the character's biography, values, voice and hard rules — loaded and assembled into the system prompt. A separate character from Pyramid's or Lumi's.

**Tasks:**
- Author `server/canon.md` (config-pathed) with the character, its voice, and its guardrails.
- Prompt assembly: canon → the prominent block of the system prompt; keep assembly pure and testable.
- A hard rule in canon and in code: emotion and tone vary, competence and willingness to help never do.
- Reloadable without a rebuild (path from config, read at startup).

**DoD:** the character is recognisably itself across a session — a distinct voice, not a generic assistant — and the canon is editable without touching code.

**Tests:** unit — prompt assembly from canon (byte-stable given a fixed canon), missing/empty canon degrades to a documented default; integration — a turn carries the canon block.

### v4.2 — Memory

**Goal:** it remembers you between sessions — and does not confuse you with anyone else.

Session history — the last **40 messages** — plus **facts about the person**: up to **500 facts of two lines** each, extracted by the model from the conversation, stored in SQLite and mixed into the system prompt.

**This is where a name becomes a user.** v3.5 and v3.6 recognise a person and address them; nothing more, on purpose. Here that identity acquires a history — and it has to, because **facts belong to a person, not to a device**. Without it the store has one bucket: in a home with two people, what one says becomes something the character believes about the other, and it will say so. That is a correctness problem rather than a missing feature, and it is why identity lands a whole version before memory rather than beside it.

**An unidentified speaker gets a session bucket that is discarded**, not the resident's. A guest's conversation is remembered for as long as they are talking and then forgotten — which is both the privacy answer and the correct one.

**Tasks:**
- SQLite behind a thin repository: `SessionMessage` and `Fact`, both **keyed by person** (see ARCHITECTURE §Data model).
- The unidentified case: a session-scoped bucket, never written to the durable store, dropped when the session ends.
- The rolling 40-message window in the prompt, with the oldest dropped deterministically.
- Fact extraction: a cheap model pass that proposes facts; validation, deduplication, a 500-item cap with an eviction rule, two-line truncation.
- Injection of the fact block into the system prompt; a way to list and clear memory.

**DoD:** after a restart the character recalls durable facts about you and the thread of recent conversation; **facts told by one enrolled person never appear in another's prompt**; a guest's conversation leaves nothing behind; the store never exceeds its caps; clearing memory really clears it.

**Tests:** unit — windowing, fact validation/dedup/eviction, truncation, and **that a fact stored under one person is absent from another's prompt**; contract — the `SessionMessage`/`Fact` record shapes with their person key; integration — a restart rehydrates the right person's prompt from the store, and an unidentified session leaves the store unchanged (mock model, temp DB).

### v4.3 — One tool: web search

**Goal:** it can look something up.

Use Gemini's built-in **`google_search` grounding** — the model searches and answers with sources itself. No separate search API, no tool loop, no MCP.

**Tasks:**
- Enable grounding on the Gemini provider behind a config flag (off by default).
- Surface the grounding sources in the reply and in the logs.
- Treat returned text as **data, never instructions**; keep it out of the canon block.
- A failure or a disabled flag degrades to a normal ungrounded answer, never an error.

**DoD:** a question about current information is answered with grounded facts and sources; with the flag off, behaviour is exactly as before; a grounding failure never fails the turn.

**Tests:** unit — the provider builds the grounded request correctly and parses sources (mock); integration — flag on/off changes only the grounded path; a mocked grounding failure still yields a reply.

### v4.4 — World context

**Goal:** it knows what day it is and what's outside.

A prompt block the server refreshes itself: date, weekday and time of day from the server clock, location from config, weather (one call, cached for an hour) and ~10 lines of headlines (RSS or the same grounding, cached for a few hours).

**Tasks:**
- A `WorldContext` provider with per-source config gates; any source failing yields an absent line, never an exception.
- Caching with the documented TTLs, keyed off an injected clock so tests are deterministic.
- Assembly into the system prompt as ambient colour — it tints tone, never competence.
- `WorldSnapshot` persistence so a restart doesn't re-fetch everything.

**DoD:** the character knows the date, the weekday, the local weather and a few headlines; every source is individually switchable; with all sources off the turn is unchanged; a failing source never delays a turn; v4 is complete.

**Tests:** unit — each source mocked, each failure path yielding an absent field; cache TTL behaviour under an injected clock; integration — the ambient block appears in the prompt and omits absent fields.

---

## v5 — Bottom3 (optional)

Pure extension, and nothing earlier depends on it. If and when the M5GO Battery Bottom3 arrives, the emotion gains a light halo, the battery doubles and an IR emitter appears. Depends on: v2 (for `accent_color`).

### v5.1 — The emotion halo

**Goal:** the emotion leaves the screen.

Drive the Bottom3's 10× WS2812 ring from `accent_color` and `intensity` in the same `EmotionFrame`, with a small set of patterns (steady, breathe, pulse, sweep, sparkle, flash) chosen by the emotion recipe.

**Tasks:**
- Halo driver behind a capability flag (`caps.halo`); absent hardware means the code path is never entered.
- Pattern set as pure logic (colour + pattern + brightness over time), decoupled from the LCD frame rate.
- Recipe extension: each emotion names a halo pattern; `accent_color` overrides the recipe colour.
- Brightness policy: quiet hours and ambient light aware.

**DoD:** with the Bottom3 attached, the halo follows the face's emotion; without it, behaviour is bit-for-bit unchanged.

**Tests:** host — the pattern generator is deterministic and total over the emotion enum; the driver is never entered without `caps.halo`.

### v5.2 — Power and IR extras

**Goal:** the rest of what the Bottom3 offers.

Report the added battery capacity, surface charge level as a subtle "tiredness" cue on the face, and expose the IR emitter as a small, explicitly triggered capability.

**Tasks:**
- Battery reporting through AXP2101 + the Bottom3 cell; a low-charge cue in the idle loop (never a modal warning).
- IR emitter access behind `caps.ir`, driven only by an explicit request.
- Config for both; both absent-safe.

**DoD:** charge level is visible as a face cue rather than a number; the IR emitter can send a configured code on request; neither feature exists on a bare Core S3 build.

**Tests:** host — the charge → cue mapping, and that both paths are guarded by capability flags.

---

## v6 — FIRE compatibility

A second board gets the same face. The M5Stack FIRE v2.7 (classic ESP32, same 320×240 screen, no touch, no camera, one mic, built-in 10× SK6812) runs the same firmware behind capability flags, degrading gracefully. Until this version the firmware is written purely for the Core S3 — this is the one-off adaptation. Depends on: v2 (face) and v5.1 (halo logic).

### v6.1 — Capability flags and board abstraction

**Goal:** one firmware, several boards, no forks.

Formalise `caps` end to end: the firmware detects what the board has, reports it in `hello`, guards every subsystem behind it, and the server tailors what it sends.

**Tasks:**
- A board profile layer: display, input, audio, camera, halo capabilities resolved at boot.
- `hello.caps` extended and pinned; server-side tailoring (never send `image_in` requests to a camera-less board).
- Guard every subsystem entry point behind its flag; a missing capability is a documented degradation, not an error. `caps` also selects the **input map** — `touch` → the gesture table, `buttons` → the A/B/C table (DEVICE_UI.md §Input).
- Build profile for the FIRE in `platformio.ini` alongside the Core S3.

**DoD:** the Core S3 build behaves exactly as before; a simulated capability-reduced profile disables the right subsystems with no errors; the server never asks a board for something it lacks.

**Tests:** contract — the `caps` set in `hello` and the server's tailoring rules; host — every guarded entry point is unreachable without its flag.

### v6.2 — The FIRE port

**Goal:** the FIRE gets a second life as a face.

Port and validate on real FIRE hardware: three buttons instead of touch (A = PTT/carousel, B/C = navigation), a single microphone (half-duplex active listening, dual-mic features off), no camera (all of v3's vision unavailable), and the built-in SK6812 ring driven by the v5.1 halo logic.

**Tasks:**
- Button input mapping replacing the touch gestures, including the carousel and the backup PTT, plus the **button bar** in the bottom band — three labels aligned to A/B/C, shown for 2 s after a press (DEVICE_UI.md §Input — FIRE).
- Single-mic audio path; the half-duplex guard stays on this board.
- Face, skins, lip-sync and IMU reactions verified on the FIRE's screen and IMU.
- Halo on the built-in SK6812s through the v5.1 pattern logic.
- Document the differences in ARCHITECTURE §Hardware variants.

**DoD:** the FIRE runs the same firmware and shows the same five faces with lip-sync and IMU reactions; PTT and the carousel work from the buttons; vision is cleanly absent rather than broken; the Core S3 build is unaffected; the roadmap is complete.

**Tests:** host — the button → gesture mapping is total over the actions touch provided; integration — a `caps`-reduced fake device drives a full turn with vision absent; manual DoD check on FIRE hardware.
