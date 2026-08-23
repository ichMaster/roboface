# Roadmap — RoboFace

Seven self-contained versions, built in order: **v0** Skeleton (server + device + the wire contract) → **v1** Voice (the full loop, hands-free) → **v2** The living face (procedural face, emotion channel, lip-sync, touch/motion, skins) → **v3** Vision (look-and-tell, presence, the background emotion channel, AEC) → **v4** The simple mind (canon, memory, one tool, world context) → **v5** Bottom3, optional (the emotion halo) → **v6** FIRE compatibility (a second board behind capability flags).

Versions are numbered from 0; phases inside a version are numbered `vA.B` (A = version, B = phase), e.g. `v2.3`. Each phase lists a **Goal**, a short description, a **Tasks** list, a **Definition of Done (DoD)** and the **Tests** that encode it (see [ARCHITECTURE.md](ARCHITECTURE.md) §Testing and CI).

**Versioning (`A.B.C`).** `A` = roadmap version (v0→0 … v6→6), `B` = phase within it (`v2.3` → `2.3.0`), `C` = a post-release fix on that phase. Roadmap phase `vA.B` → release `A.B.0`, tagged `vA.B.0`; a fix after it bumps `C`. Releases are cut per phase. **Never bump the version without explicit confirmation.**

Arc of the two axes: the **device** grows screen → speaker → microphone → touch/IMU/proximity → camera → halo/second board; the **server** grows protocol → orchestrator → emotion engine → vision → mind. The device stays thin throughout — every capability added to it is I/O, never a decision. Complexity is added only by version, never all at once.

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
- `orchestrator.py`: run a text turn, stream deltas as `reply{text, final}`, map failures to `llm_timeout` / `llm_failed` / `rate_limited`.
- `prompt.py`: assemble a minimal system prompt (a placeholder character until v4.1).

**DoD:** a `text_in` from the fake device returns a real Gemini answer, streamed in deltas, with a per-stage timeout; every provider failure surfaces as an enumerated `error`; no test makes a paid call.

**Tests:** contract — the `LLMProvider` interface and the `reply` delta shape; unit — prompt assembly, error mapping, timeout handling; integration — a full `text_in → reply` turn against the mock.

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
- Screen brightness from config; the screen never shows raw error text — an error maps to the error face plus a code in the serial log.

**DoD:** the board shows a distinct face for `idle`, `listening`, `thinking`, `replying`, `offline` and `error`; the renderer interface is the only way the app draws a face; v0 is a complete, self-contained skeleton.

**Tests:** host — the state → face-recipe mapping is total over the state enum; manual DoD check on hardware (each state visibly distinct, no tearing).

---

## v1 — Voice

The complete voice loop through the server, hands-free by the end. Output is built before input (validate the easy path first): TTS playback, then capture, then ASR, then VAD-driven active listening. At the end of v1 you can hold a spoken conversation with a face that still only shows states — the face itself comes in v2. Depends on: v0.

### v1.1 — Streaming TTS playback

**Goal:** typed text comes back as a voice.

Add the **`TTSProvider` seam** and the ElevenLabs streaming adapter (`pcm_16000`), the server-side sentence buffering that hands completed clauses to TTS, and device-side playback: `tts_audio` binary frames into a PSRAM ring buffer feeding the AW88298 amp, closed by `tts_end`.

**Tasks:**
- `providers/base.py`: `TTSProvider.synthesize(text) -> AsyncIterator[bytes]` (PCM16 16 kHz mono) + a mock.
- `providers/elevenlabs.py`: streaming synthesis, the character's voice id from config, `tts_failed` mapping.
- `sentence.py`: buffer LLM deltas to clause/sentence boundaries; hand each phrase to TTS as it completes.
- Firmware `audio_io`: a PSRAM ring buffer, I2S playback, underrun handling, `tts_end` completion.
- Volume from config; playback cleanly interruptible by a `restart`/error.

**DoD:** a line typed over serial is answered **out loud** on the device, with speech starting before the model finishes generating; an interrupted or failed synthesis ends the turn cleanly and returns to `idle`.

**Tests:** contract — `TTSProvider` and the `tts_audio`/`tts_end` framing; unit — clause boundary splitting (including abbreviations and numerals), ring-buffer underrun; integration — `text_in → tts_end` against mocks with a fake device asserting frame order.

### v1.2 — Microphone capture and push-to-talk

**Goal:** the device can send what it hears.

ES7210 capture at 16 kHz PCM16, one channel for now, streamed as binary `audio` frames between `listen_start` and `listen_stop`. Touch-and-hold on the face is the trigger — the backup PTT that survives all later versions.

**Tasks:**
- Firmware `audio_io` capture path: I2S in, 16 kHz mono PCM16, PSRAM buffering, mic/speaker mode switching.
- Touch (FT6336U): press-and-hold starts `listen_start` + streaming, release sends `listen_stop`.
- Server `router`: accept binary `audio` frames only in the `listening` state; enforce a maximum utterance length.
- Loopback diagnostic: capture N seconds and play it back locally (a hardware smoke check, not a shipped feature).

**DoD:** holding the face records speech and streams it to the server, which reports the received duration and byte count; releasing ends the utterance; oversize utterances are cut with a clean error.

**Tests:** host — the capture buffer math and the PTT state transitions; contract — binary `audio` frames only valid inside `listen_start`/`listen_stop`; integration — the fake device streams a canned PCM file and the server assembles it intact.

### v1.3 — ASR and the full voice loop

**Goal:** speak, and be answered by voice.

Add the **`ASRProvider` seam** with the Deepgram nova-2 adapter (Ukrainian, PCM16 16 kHz), interims as `asr_partial` and one final `asr`, and wire the orchestrator so a single turn streams end to end: ASR → LLM → TTS.

**Tasks:**
- `providers/base.py`: `ASRProvider.transcribe(audio) -> AsyncIterator[ASRChunk{text, is_final}]` + a mock.
- `providers/deepgram.py`: streaming recognition, Ukrainian, `asr_failed` mapping.
- Orchestrator: the full pipeline with per-stage timeouts and cancellation on device disconnect.
- Firmware: render `asr_partial`/`asr`/`reply` to the serial debug channel; the screen still shows states.
- Latency instrumentation: log `listen_stop → first tts_audio`.

**DoD:** press and hold, speak Ukrainian, and hear a spoken answer; the measured first-audio latency after `listen_stop` is **< ~1.5 s** on the development network; each stage's failure maps to its own error code.

**Tests:** contract — `ASRProvider` and the `asr_partial`/`asr` frames; unit — stage timeout and cancellation; integration — `audio → tts_end` end to end against mocks, plus the three failure paths (ASR, LLM, TTS).

### v1.4 — Active listening (VAD)

**Goal:** hands-free — no button at all.

Make **active listening the default**: the mic is always open and an on-device VAD endpointer marks speech start and end-of-utterance by pause. Half-duplex is enforced (listening pauses while the device speaks) until AEC arrives in v3.4; touch-and-hold remains as the backup PTT.

**Tasks:**
- Pure-logic VAD endpointer: energy/zero-crossing based, with a configurable pre-roll, minimum-speech and end-pause; entirely host-testable.
- Wire it to the turn state machine: speech start → `listen_start`, endpoint → `listen_stop`.
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
- Touch zones over the face (cheek/forehead/eyes) and gesture recognition: tap, multi-tap, stroke, poke, long press.
- IMU (BMI270) gestures: tilt, shake, picked up, upside down, free fall — all pure logic over the raw stream.
- Proximity (LTR-553): approach wakes the face and biases `gaze`; leave relaxes it.
- The reflex layer: a short animation composited over the current expression, bounded and non-blocking.
- `event{type, kind, meta}` to the server; server-side handling that may answer with an `emotion` frame and/or a spoken line.
- Long press reserved for the v2.6 carousel; press-and-hold remains the backup PTT.

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
- `config_updated{face_set}` handling server-side and on the device; long-press carousel; the active skin reported in the next `hello`.

**DoD:** all five faces (Stack-chan + four spirits) render the same `EmotionFrame` correctly and are switchable both ways; adding a skin requires no renderer code change; a corrupt or missing pack degrades to the procedural face instead of failing; v2 is the complete living face.

**Tests:** contract — the skin manifest schema and the `config_updated{face_set}` frame; host — manifest loading, validation and fallback, and that each pack's layer keys are total over the emotion enum; manual DoD check of the four skins on hardware.

---

## v3 — Vision

The camera opens, and with it the asynchronous emotional read of the person in front of the device. The version ends with the hardest audio work — the esp-sr front end that finally makes listening full-duplex. Depends on: v2.

### v3.1 — Look and tell

**Goal:** "what do you see?" is answered.

Capture a JPEG from the GC0308 on demand, announce it with `image_in` and send it as a binary frame; the server attaches it to that turn's Gemini call as multimodal input and answers by voice through the normal pipeline.

**Tasks:**
- Firmware camera bring-up: init, capture, JPEG encode into PSRAM at a modest resolution, memory bounded.
- Protocol: `image_in{reason, w, h}` followed by the binary JPEG; size limits and rejection rules.
- Server: attach the image to the LLM call as multimodal input; map failures to `vision_failed`.
- Trigger: an explicit request in the turn (recognised server-side) asks the device for a frame.
- Privacy: the frame lives in memory only, and is dropped as soon as the turn ends.

**DoD:** asking what it sees produces a spoken description of what is actually in front of the camera; no frame is ever written to disk; an oversized or failed capture ends the turn with `vision_failed` and a visible error face.

**Tests:** contract — the `image_in` frame and the binary JPEG rule; unit — size/dimension guards, the multimodal call shape against a mock; integration — `image_in → reply` end to end with a fixture image.

### v3.2 — Presence

**Goal:** the face wakes when someone sits down.

Cheap local motion/brightness detection on the camera, combined with the proximity sensor, wakes the face and lets the character greet someone arriving. No identity, no recognition — only "someone is there".

**Tasks:**
- Low-cost on-device presence detection (frame difference / brightness change) with hysteresis and a cooldown.
- Presence state feeds the face (wake, gaze toward the person) and is reported to the server.
- Server-side greeting policy: at most one greeting per arrival, quiet-hours aware, never during a turn.
- Config gate: presence mode is off by default and enabled explicitly.

**DoD:** sitting down in front of an idle device wakes the face and can produce a greeting; leaving and returning does not spam greetings; with presence off, the camera stays dark outside explicit turns.

**Tests:** host — the detector's hysteresis/cooldown against frame fixtures; unit — the greeting policy under an injected clock; integration — presence events reaching the server produce at most one greeting.

### v3.3 — The background emotion channel

**Goal:** the character notices how you feel — without ever slowing down the conversation.

A **separate channel and a separate Gemini call**, fully outside the turn pipeline. While presence mode is on, the device sends a small frame every few seconds; a background task asks for the person's emotion (enum + intensity) and puts the answer in two places: a **mood line** in the next turn's system prompt, and optionally an **immediate mirror** on the face.

**Tasks:**
- Device: periodic small-frame capture in presence mode, rate-limited, dropped when the link is busy.
- Server: a background task with its own concurrency limit and its own timeout, never awaited by a turn.
- The read → a validated `{emotion, intensity}`; stale reads expire.
- Injection: a one-line mood note in the next turn's prompt; optional mirroring via an out-of-turn `EmotionFrame`.
- Hard rule enforced in code: a late or failed read is dropped — turn latency must be unchanged.

**DoD:** with presence on, the character's tone adapts to a visibly happy or sad person, and the face can mirror it without a word; measured turn latency with the channel on is indistinguishable from with it off; disabling the channel changes nothing else.

**Tests:** unit — the background task never blocks a turn (a deliberately slow mock read leaves turn timing unchanged), read validation and expiry; integration — the mood line appears in the next prompt and only there; contract — the background read is not part of the turn's frame sequence.

### v3.4 — AEC and barge-in

**Goal:** you can interrupt the character mid-sentence.

Integrate the Espressif audio front end (esp-sr AFE: AEC + noise suppression + VAD) using the playback reference channel from the ES7210, so the microphone no longer hears the speaker. Active listening becomes full-duplex and the half-duplex rule is retired.

**Tasks:**
- Bring esp-sr into the build; wire the playback reference channel into the AFE.
- Replace the v1.4 endpointer's front end with the AFE's VAD while keeping the same pure-logic turn semantics.
- Barge-in policy: detected speech during playback pauses TTS output, cancels the remaining synthesis, and starts a new utterance.
- Server-side cancellation: an in-flight turn is cancelled cleanly on barge-in, with no orphaned audio frames.
- Remove the half-duplex guard behind a capability flag (boards without AEC keep it).

**DoD:** speaking while the device is talking interrupts it and starts a new turn; the device does not trigger on its own voice; on a board without AEC the half-duplex behaviour is unchanged; v3 is complete.

**Tests:** host — the barge-in decision logic against fixtures with playback bleed; integration — a barge-in cancels the in-flight turn server-side and no `tts_audio` arrives after cancellation; manual DoD check on hardware.

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

**Goal:** it remembers you between sessions.

Session history — the last **40 messages** — plus **facts about the interlocutor**: up to **500 facts of two lines** each, extracted by the model from the conversation, stored in SQLite and mixed into the system prompt.

**Tasks:**
- SQLite behind a thin repository: `SessionMessage` and `Fact` (see ARCHITECTURE §Data model).
- The rolling 40-message window in the prompt, with the oldest dropped deterministically.
- Fact extraction: a cheap model pass that proposes facts; validation, deduplication, a 500-item cap with an eviction rule, two-line truncation.
- Injection of the fact block into the system prompt; a way to list and clear memory.

**DoD:** after a restart the character recalls durable facts about you and the thread of recent conversation; the store never exceeds its caps; clearing memory really clears it.

**Tests:** unit — windowing, fact validation/dedup/eviction, truncation; contract — the `SessionMessage`/`Fact` record shapes; integration — a restart rehydrates the prompt from the store (mock model, temp DB).

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
- Guard every subsystem entry point behind its flag; a missing capability is a documented degradation, not an error.
- Build profile for the FIRE in `platformio.ini` alongside the Core S3.

**DoD:** the Core S3 build behaves exactly as before; a simulated capability-reduced profile disables the right subsystems with no errors; the server never asks a board for something it lacks.

**Tests:** contract — the `caps` set in `hello` and the server's tailoring rules; host — every guarded entry point is unreachable without its flag.

### v6.2 — The FIRE port

**Goal:** the FIRE gets a second life as a face.

Port and validate on real FIRE hardware: three buttons instead of touch (A = PTT/carousel, B/C = navigation), a single microphone (half-duplex active listening, dual-mic features off), no camera (all of v3's vision unavailable), and the built-in SK6812 ring driven by the v5.1 halo logic.

**Tasks:**
- Button input mapping replacing the touch gestures, including the carousel and the backup PTT.
- Single-mic audio path; the half-duplex guard stays on this board.
- Face, skins, lip-sync and IMU reactions verified on the FIRE's screen and IMU.
- Halo on the built-in SK6812s through the v5.1 pattern logic.
- Document the differences in ARCHITECTURE §Hardware variants.

**DoD:** the FIRE runs the same firmware and shows the same five faces with lip-sync and IMU reactions; PTT and the carousel work from the buttons; vision is cleanly absent rather than broken; the Core S3 build is unaffected; the roadmap is complete.

**Tests:** host — the button → gesture mapping is total over the actions touch provided; integration — a `caps`-reduced fake device drives a full turn with vision absent; manual DoD check on FIRE hardware.
