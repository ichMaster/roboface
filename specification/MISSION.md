# Mission — RoboFace

## In one sentence

RoboFace is a desktop AI companion with a **living animated face** on an M5Stack Core S3 — a thin device that renders emotion, hears, speaks and sees, driven entirely by a server that does the thinking.

## What we are building

A small robot head that sits on a desk and is *present*. It blinks, breathes and drifts its gaze in silence; it listens without being asked; it answers in a voice; its face carries the emotion of the answer rather than a status label. Touch it and it reacts before the network does; tilt it and its eyes keep the horizon; show it something and it looks and tells you what it sees.

Everything that decides — the persona, the emotion, the memory, the vision reasoning — lives on a **server** written from scratch in Python. The **device** is deliberately thin: it renders the frame it is given, streams audio in and out, sends camera frames, and reports what it senses. Two things stay on the device for latency alone: the **lip-sync** (computed from the TTS audio it is already playing) and the **local reflexes** to touch, motion and proximity (<100 ms of animation over the current state).

The face is the product. It ships as a ladder: first a simplified procedural face in the Stack-chan style, then four sprite "spirit" skins — ghost, flame, jellyfish, cloud — all switchable over one and the same `EmotionFrame`. A new skin is a set of shapes and a palette, never new renderer logic.

## For whom

A private project for its author, on hardware already owned (an M5Stack Core S3 now, an M5Stack FIRE v2.7 in v6). It is a companion on a desk, not a product, not a service, and not something a stranger connects to. Access is one device and one server on a local network with keys in a local `.env`.

## Principles

- **Intelligence off-device.** The server decides emotion, persona and meaning; the firmware renders and reports. Never put persona logic or an emotion decision into the device.
- **Two exceptions, both for latency.** Lip-sync is derived on-device from the audio being played (the server sends nothing for it), and touch/IMU/proximity reflexes fire locally in under 100 ms without interrupting speech or listening. Everything else waits for the server.
- **One contract, many faces.** `EmotionFrame` is the whole face channel. Renderer tiers and skins change; the contract, the emotion enum and the renderer interface do not.
- **Simplicity first, by version.** Complexity is added only by version. Each version is self-sufficient and ships alone; the hardware map assigns every sensor to a version on purpose.
- **Chat is Gemini, and only Gemini.** The `LLMProvider` seam has exactly one real implementation — **Gemini 2.5 Flash with thinking disabled** (`thinkingBudget: 0`) — chosen for the lowest time-to-first-token and for multimodal input that covers the vision turn and the background emotion read with no second vendor. Speech is not chat: ASR is Deepgram, TTS is ElevenLabs, each behind its own seam.
- **A separate project from Pyramid.** Pyramid (the author's AtomS3R device) is the conceptual model — the same protocol/router/orchestrator/providers shape, the same testing discipline — but **no shared code**. The server is written from scratch here.
- **The mind is deliberately small.** Canon, session memory, facts, one tool and a world-context block (v4). Pyramid's roadmap of role consoles, MCP, long-term memory and temperament is not carried over.
- **Streaming everywhere.** Every stage of a turn streams and each starts on the first output of the one before it: the mic uploads while you speak, recognition runs over a WebSocket so the transcript is ready when you stop, model deltas leave as they arrive, each completed phrase is synthesized immediately, and the device plays audio as it lands. The test of it is that the first audio leaves **before the reply is finished**. Perceived latency is a property of the architecture, not a tuning pass at the end.
- **Contracts are pinned by tests.** The wire protocol, `EmotionFrame`, the provider seams and the skin manifest each have a contract test. Changing a contract changes its test in the same commit.
- **No paid APIs in tests.** A fake device drives the protocol and every provider is mocked; a live call is opt-in and never the default suite.
- **Privacy by construction.** Camera frames are processed in memory and never stored. The camera is live only in an explicit turn or in presence mode, and presence mode is a role-level opt-in.

## Non-goals

- No cloud service, no accounts, no multi-user, no public sign-up — one owner, one device.
- No second chat vendor. No OpenAI/Anthropic/local-model adapters behind the `LLMProvider` seam.
- No MCP, no role console, no long-term "temperament", no facet engine — those live in Pyramid and Lumi, not here.
- No on-device intelligence: no local wake-word model beyond VAD/AFE, no on-device emotion inference, no persona in firmware.
- No face recognition or identity tracking from the camera. Presence and emotion, never *who*.
- No photorealism. At 320×240 the face is a stylized emotion renderer, not a portrait.

## Glossary

- **Device / firmware** — the M5Stack Core S3 (ESP32-S3, 320×240 touch LCD, camera, dual mics, speaker, IMU, proximity). From v6 also the M5Stack FIRE v2.7 behind capability flags.
- **Server** — the Python process that owns the turn: ASR → LLM → TTS orchestration, the emotion engine, vision, and (from v4) the mind.
- **Turn** — one exchange: the user's utterance (or text, or an event) through ASR → LLM → TTS back to the device. Half-duplex until v3.4.
- **`EmotionFrame`** — the server→device face contract: `{emotion, intensity, gaze, accent_color, speaking, ttl_ms}`. Emitted per turn and per state change.
- **Emotion enum** — the small fixed set the face renders: `neutral, calm, joy, thinking, surprised, sad, error`.
- **Skin / `face_set`** — one visual implementation of the face over the shared contract: `stackchan` (procedural, tier 1) plus `ghost`, `flame`, `jelly`, `cloud` (sprite skins, tier 2). Switched from the server (`config_updated`) or by a long press.
- **Renderer ladder** — stub face (v0) → procedural Stack-chan (v2) → spirit sprite skins (v2.6), all behind one `IFaceRenderer`.
- **Reflex** — a local, sub-100 ms animation on the device in response to touch, motion or proximity, over the current state, never interrupting speech or listening.
- **`event{}`** — the device→server report of that same touch/motion, so the character may also answer with a line or an emotion change (the second level of the reaction model).
- **Active listening** — the default input mode: the mic is always open and a VAD endpointer cuts the utterance. Touch-and-hold is the backup PTT.
- **Barge-in** — interrupting the character by voice while it speaks; requires AEC (v3.4) and ends the half-duplex rule.
- **Presence mode** — the camera's low-cost motion/brightness watch (plus the proximity sensor) that wakes the face when someone sits down.
- **Background emotion channel** — the asynchronous, separate Gemini call that reads the user's emotion from an occasional frame and feeds a mood line into the next turn's prompt. It never blocks or slows a turn.
- **Canon** — the authored text file describing the RoboFace character, assembled into the system prompt (v4.1). A separate character, not Pyramid's or Lumi's.
- **World context** — the server-refreshed prompt block carrying date/time, location, weather and a few headlines (v4.4).
- **Halo** — the 10× WS2812 ring on the optional M5GO Battery Bottom3 (v5), driven by `accent_color` from the same `EmotionFrame`. On the FIRE (v6) the same logic drives its built-in SK6812s.
- **Capability flags** — the firmware's description of what the current board has (touch, camera, dual mics, halo), so one build degrades gracefully across boards.
- **Codegen tracking** — `codegen/`: the event log, hooks and dashboard that record how this repository is generated by the SDLC skills. Subject-independent; never imported by the product.
