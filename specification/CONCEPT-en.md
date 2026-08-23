# RoboFace — Concept (v0.1)

A desktop AI companion with a **living animated face** on the M5Stack **Core S3**.
A conceptual successor of pyramid (NOT a fork — all code is new): the same
three-tier architecture and a similar WS contract, but **server-side from day
one** (no direct device→cloud stage), focused on what the AtomS3R could not do —
a **320×240 touch screen and a camera**. The device stays thin: face, audio I/O
and camera — all intelligence (ASR→LLM→TTS, emotions, vision) lives on the
server.

```
Core S3 (face + audio + camera)
   <-> WiFi / WSS: JSON + PCM16 + JPEG
Server (Python: ASR->LLM->TTS orchestrator, emotion engine, vision)
   <-> HTTPS
LLM / ASR / TTS endpoints (LLM: Gemini 2.5 Flash, no thinking; ASR/TTS: Deepgram, ElevenLabs — same as pyramid)
```

## Hardware

**M5Stack Core S3:** ESP32-S3 (240 MHz, 16 MB flash, 8 MB PSRAM), 2" IPS
320×240 capacitive touch (ILI9342C + FT6336U), **GC0308** camera (0.3 MP),
**dual microphones** (ES7210), AW88298 amp (1 W) + speaker, LTR-553
proximity/ambient-light sensor, BMI270 IMU (+BMM150 magnetometer), RTC,
microSD, 500 mAh battery, AXP2101 PMU.

**Base decision:** the core versions run on the **bare Core S3** — its own
500 mAh battery is enough for a desktop scenario. The M5GO Battery Bottom v1 is
incompatible with Core S3 (it is for the old Core/Fire) and is not used in this
project. The **M5GO Battery Bottom3** (A014-D: +500 mAh, 10× WS2812 "halo", IR
emitter, 2 Grove ports) is an **optional extension of a later version**:
everything that needs it is marked optional and blocks no base phase. If more
battery life is ever needed, it can be added with the **Battery Module 13.2
(1500 mAh)** — a pure battery module of the 13.2 series on the BAT rail (Seiko
protection board, zero GPIOs — no pin conflicts) that stacks on at any moment
with no firmware changes.

Hardware-to-version map:

| Core S3 hardware | Use in roboface | When |
|---|---|---|
| WiFi (ESP32-S3) | WSS channel to the server — the backbone | v0 |
| USB-C serial | debug channel and text_in, as in pyramid | v0 |
| 2" 320×240 display (ILI9342C) | turn states → full animated face from v2 | v0 stub, v2 fully |
| 8 MB PSRAM | audio buffers, face sprite framebuffer, JPEG frames | v1–v3 (foundation) |
| AW88298 speaker (1 W) | streaming TTS playback | v1 |
| Dual mics (ES7210) | 16 kHz PCM16 capture; voice direction for gaze, cleaner ASR signal, robust VAD, AEC+barge-in | v1 mono; v2 direction+ASR; v3+ AEC |
| Touch (FT6336U) | backup PTT ("hold the face"), touch reactions | v1 PTT, v2 reactions |
| LTR-553 proximity | wake on approaching hand, gaze toward the hand | v2 |
| BMI270 IMU | tilt/shake reactions (surprise, "dizziness") | v2 |
| GC0308 camera (0.3 MP) | "look and tell", presence mode | v3 |
| Ambient light (LTR-553) | auto brightness of the face day/night | later |
| BMM150 magnetometer | compass tricks ("where is north?") — low priority | later |
| BM8563 RTC | time without network, persona routines | later (with v4 memory) |
| microSD | face sprite pack cache, local logs | later (authored sprites) |
| Grove ports A/B/C | external sensors/extensions | later, as needed |
| AXP2101 + 500 mAh battery | power; charge level as "tiredness" on the face | v0 power, indication later |
| BLE 5 (ESP32-S3) | not used (WiFi-first); maybe WiFi provisioning someday | out of plan |

In the first versions (v0–v2) the whole "body" works: screen, audio both ways,
touch, proximity, IMU. The camera is the only large block deferred to v3; RTC,
microSD, ambient light and Grove attach later without re-architecting. Later,
with the optional Bottom3 — a light halo in sync with the emotion.

## The face — the main feature

Same contract as designed in pyramid (EMOTION_FACE): **the server decides the
emotion**, the device only renders
`EmotionFrame{emotion, intensity, gaze, accent_color, speaking, ttl_ms}`.
Rendering is a ladder: **first a simplified Stack-chan-style face** (procedural
eyes/mouth, as in M5Stack-Avatar — which is the reference and a possible base),
**then four "spirit" sprite skins: ghost, flame spirit, jellyfish and cloud**.
All five faces (Stack-chan + four spirits) are **switchable skins** on top of
one and the same EmotionFrame: switching from the server config (`face_set` via
`config_updated`) or by a gesture on the device (long press — face carousel).
The spirits express emotion also through their "element": the flame changes
color, the cloud changes weather (sun/rain/lightning), the jellyfish changes its
glow, the ghost blushes and sheds a tear. The first tier:

- **Idle loop:** blinking, "breathing", slow gaze drift — the face is alive
  even in silence.
- **Lip-sync:** the mouth animates from the **amplitude envelope of the TTS
  audio** the device is already playing (no extra data from the server) —
  4–5 mouth heights, smoothing, ~15–20 FPS over layers via M5GFX sprites.
- **Emotions:** a small enum (joy, calm, thinking, surprised, sad, error…) →
  a layer recipe; `accent_color` from the same EmotionFrame will later drive
  the halo (optional, Bottom3). Turn states (listening / thinking / replying)
  are also expressed by the face, not by text labels.
- **Gaze:** `gaze` from the server, plus locally toward touch and the proximity
  sensor (looks at an approaching hand).
- Later — the spirit sprite skins (ghost, flame, jellyfish, cloud) over the
  same layers: a pure asset swap; every new skin is just shapes and a palette,
  no logic changes.

## Camera

The camera is the "eyes" of the same face, three steps:

1. **Look and tell (first):** on request ("what do you see?") the device sends
   a JPEG frame to the server, the server adds it to the LLM turn (multimodal)
   — the answer is spoken. One new frame type in the protocol: `image_in`
   (binary JPEG with a short JSON prefix or a separate control message).
2. **Presence:** cheap local motion/brightness detection + the proximity
   sensor → the face "wakes up", greets when someone sits nearby. No face
   recognition at the start.
3. **User-emotion recognition — right away, asynchronously.** A **separate
   channel and a separate LLM call**, fully outside the main turn pipeline:
   when presence mode sees a person, the device periodically (every few
   seconds) sends a small JPEG frame as a background message; the server, in a
   background task, makes a cheap multimodal call to the same Gemini Flash
   ("what is the person's emotion?" → enum + intensity) and puts the result in
   two places: a **user-mood line** in the next turn's system prompt (the
   persona adapts its tone) and, optionally, **instant mirroring** on the face
   via EmotionFrame (empathy without a single word). This channel never blocks
   or slows the conversation — if the background call is late, the turn goes
   on without it. Wake-on-face is a natural side effect of the same presence
   channel.

Privacy: frames are processed in memory and never stored; the camera is active
only in an explicit turn or in presence mode, which is enabled per role.

## Server

RoboFace is a **separate project** — not a branch and **not a fork** of
pyramid. The server is **written from scratch**: pyramid serves only as a
conceptual model — the same build idea (a pure protocol module as the contract,
a router with connection states, an orchestrator streaming every stage,
providers with mocks for CI, contract tests), but all code is new and made for
this project. It includes:

- `EmotionFrame` in the outbound contract (mandatory here from the first face
  version);
- the vision turn (JPEG intake, multimodal LLM);
- ASR and TTS — **the same services as pyramid**: recognition — **Deepgram**
  (nova-2, Ukrainian, PCM16 16 kHz), voice synthesis — **ElevenLabs**
  (streaming, `pcm_16000`); the adapters are written anew (pyramid is only a
  reference), with a separate voice for the new character;
- the LLM provider — **Gemini 2.5 Flash with thinking disabled**
  (`thinkingBudget: 0`): minimal first-token time and cost; deep reasoning is
  not needed for a short conversational character; multimodal out of the box
  also covers the v3 vision turn. An `LLMProvider` interface allows adding the
  adapter without touching the orchestrator.

The "mind" here is deliberately **very simple** — the pyramid roadmap
(Role console, MCP, long-term memory, temperament) is not carried over.

## Interaction

**Touch and motion — two levels of reaction.** Level one — **instant reflexes
on the device** (<100 ms, pure animation over the current state, never
interrupting speech or listening). Level two — the event goes to the server as
a new message `event{type: touch|motion, kind}`, and the character may also
react with a spoken line or an emotion change (the server returns an
EmotionFrame / TTS).

Touch (face touch zones):

- a short tap on the cheek/forehead — "tickles": a wink, a light smile;
  several taps in a row — growing joy and blush;
- a slow stroke (swipe) — pleasure: arc-closed eyes, blush;
- a poke in the eye — surprise, blinking and a small recoil;
- a long press — the skin carousel; press-and-hold — backup PTT.

Motion (BMI270 IMU):

- tilting the body — the eyes "roll" against the tilt (gaze keeps the
  horizon); a sharp tilt — light surprise;
- shaking — "dizziness": spiral eyes and wobble; prolonged shaking — offense
  (sadness) and a spoken line via the server;
- picked up / being carried — alert wide eyes, looking around;
- turned upside down — the face flips and frowns;
- free fall — fright (maximum surprise) full screen.

By default — **active listening**: the microphone always listens, VAD cuts out
the utterance (speech start → end by pause; the endpointer already exists in
pyramid). Half-duplex at the start: while the device itself is speaking,
listening is paused — until AEC arrives (below). Touch is the backup PTT
("hold the face") and the "petting" channel; USB-serial remains the debug
channel, as in pyramid.

**Dual microphones (ES7210) — four features,** all planned:

1. **AEC + barge-in (v3+).** The ES7210 captures a reference channel from the
   playback path; esp-sr (Espressif's audio front end: AEC + noise suppression
   + VAD) subtracts what the speaker is playing from the mic signal. Active
   listening becomes full-duplex — the character can be interrupted by voice,
   half-duplex disappears. The hardest integration, hence last.
2. **Voice direction → gaze (v2).** From the time/level difference between the
   mics — a rough direction (left/right), and the face turns its gaze toward
   the speaker via `gaze` in EmotionFrame. Pure math, no libraries.
3. **Robust VAD (v2–v3).** Dual-channel processing distinguishes directed
   speech from diffuse noise (TV, street) — fewer false triggers of active
   listening.
4. **Cleaner signal for ASR (v2).** Picking the better channel or summing the
   two — better recognition in noise. Stereo is processed on the device; the
   server receives ready-made **mono** 16 kHz (no doubled traffic).

Rollout: v1 — one channel (simple); v2 — features 2 and 4 (+start of 3);
v3+ — the esp-sr AFE with AEC and barge-in.

## Versions (the skeleton; each is self-sufficient)

- **v0 — Skeleton.** A new server from scratch (modeled on pyramid) + Core S3
  as a WSS client: text_in → reply, a static face stub with states. Contract
  tests green.
- **v1 — Voice.** The full voice loop through the server with **active
  listening** (VAD; touch as backup PTT), streaming TTS playback. Effectively
  parity with pyramid v2.1, but on the S3 and hands-free.
- **v2 — The living face (Stack-chan style).** A simplified procedural face:
  the idle loop, lip-sync, EmotionFrame from the server — and all of the
  "interaction": the two-level model from "Interaction" (local reflexes to
  touch/proximity/IMU + `event{}` messages to the server with spoken
  reactions). Then — the four spirit sprite skins (ghost, flame, jellyfish,
  cloud) with switching between them and Stack-chan (carousel by long press).
- **v3 — Vision.** "Look and tell", presence mode and asynchronous
  user-emotion recognition (separate channel + separate background LLM call;
  mood into the prompt and mirroring on the face).
- **v4 — The simple mind.** Three parts, all on the server, no console:
  - **Canon:** a simple text canon of the separate RoboFace character (not
    Lili/Pyramid) — one file, assembled into the system prompt.
  - **Memory:** session history — the last **40 messages**; plus **facts about
    the interlocutor** — up to **500 facts of 2 lines** each, gathered from
    the conversation automatically by the LLM, stored in SQLite and mixed into
    the system prompt.
  - **One tool — web search:** a Google-style query and a ready answer from
    Google AI. The natural implementation — the built-in **google_search
    grounding** of Gemini 2.5 Flash: the model searches and answers with
    sources itself; no separate search API needed.
  - **World context** — a block in the system prompt the server refreshes on
    its own: **calendar and time** (date, weekday, time of day — from the
    server clock), **location** (from config), **weather** (one API call,
    cached for an hour) and **short news, 10 lines** (headlines from RSS or
    the same google_search, cached for a few hours). The character always
    knows what day it is, what's outside and what's happening in the world.

  MCP, the Role model and the temperament are deliberately absent in v4, but
  not crossed out — they will be added to the project in later versions.
- **v5 — Bottom3 (optional).** If/when the Bottom3 arrives: an emotion halo
  driven by `accent_color`, +500 mAh, an IR emitter. A pure extension — no
  earlier phase depends on it.
- **v6 — FIRE compatibility.** A firmware port to the **M5Stack FIRE v2.7**
  (classic ESP32, same 320×240 screen, no touch and no camera) with graceful
  degradation via capability flags: the face, skins, lip-sync and IMU
  reactions — fully; three buttons instead of touch (A — PTT/carousel, B/C —
  navigation); a single microphone — active listening stays half-duplex and
  the dual-mic features are disabled; all of "Vision" (v3) is unavailable;
  the halo works right away on the built-in 10× SK6812 (same logic as v5).
  Until v6 the firmware is written purely for the Core S3, with no regard for
  the FIRE — the adaptation is a one-off job within this version. The FIRE is
  already owned and gets a second life after the port — as a face to mount on
  the author's own robotics projects.

## Decisions taken

No open questions — all agreed:

1. Base versions run on the bare Core S3, no bottom; Bottom3 is the optional
   v5, Bottom v1 is not used; extra battery life if needed — Battery Module
   13.2.
2. RoboFace is a separate project from pyramid: the server is written from
   scratch, pyramid is only a conceptual model, zero shared code; the "mind"
   is deliberately simple (v4).
3. Default listening mode — active listening with VAD (touch as backup PTT).
4. The face — first simplified Stack-chan style; then the spirit sprite skins:
   ghost, flame spirit, jellyfish, cloud — with the ability to switch skins
   (including Stack-chan) from the server or by gesture.
5. The persona — a separate RoboFace character, not Lili/Pyramid.
