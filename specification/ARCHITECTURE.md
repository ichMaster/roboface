# Architecture — RoboFace

## Overview

Three tiers. The **device** renders the face, moves audio in both directions and captures frames. The **server** owns every decision — it runs the turn (ASR → LLM → TTS), decides the emotion, does the vision reasoning and, from v4, holds the mind. **External services** are reached only from the server over HTTPS.

```
M5Stack Core S3 (face + audio I/O + camera + sensors)
   ⇅  WiFi / WSS — JSON control frames + binary PCM16 + binary JPEG
Python server (protocol · router · orchestrator · emotion engine · vision · mind)
   ⇅  HTTPS
Gemini 2.5 Flash (chat + vision, thinkingBudget: 0) · Deepgram nova-2 (ASR, uk) · ElevenLabs (TTS, pcm_16000)
```

The split is the whole design: **no intelligence on the device, no rendering decisions on the server.** The server never sends pixels; the device never chooses an emotion.

Two companion documents carry detail this one only summarises: **[features/DEVICE_UI.md](features/DEVICE_UI.md)** — everything on the screen that is not the face (indicators, input map per board, notification ranks, full-screen states), prototyped in [device-ui-prototype.html](device-ui-prototype.html); and [face-prototype.html](face-prototype.html) — the executable reference for the face renderer itself.

## Components

- **Firmware (`firmware/`).** C++ on the Core S3 (PlatformIO, M5Unified/M5GFX). Owns the WSS client, the device state machine, audio capture and playback, the camera, the sensors, and the face renderer with its skins. Split in two layers (§Firmware architecture): Arduino-free **pure logic** that is host-tested, and **glue** that touches hardware.
- **Server (`server/`).** Python, FastAPI + websockets. A pure **`protocol`** module (the wire contract), a **`router`** (one connection state machine per device), an **`orchestrator`** (streams every stage of a turn), an **emotion engine** (decides `EmotionFrame` from the turn and the model's own reported state), **`providers/`** (one seam per external service, each with a mock), and from v4 **memory** and **world context** over SQLite.
- **Assets (`assets/`).** Face skin packs and their manifest. Adding a skin is adding a directory and a manifest entry.
- **External services.** Chat and vision: **Gemini 2.5 Flash only**. ASR: **Deepgram** (nova-2, Ukrainian, PCM16 16 kHz). TTS: **ElevenLabs** (streaming `pcm_16000`). Each behind its provider seam; keys live in `server/.env`.
- **Codegen (`codegen/`).** The generation tracker, hooks and dashboard. Subject-independent: it observes how this repo is built and is never imported by the product.

## Hardware map

Every piece of Core S3 hardware is assigned to a version on purpose; nothing is "wired up later, somehow".

| Hardware | Use | Version |
|---|---|---|
| WiFi (ESP32-S3) | the WSS channel — the backbone | v0 |
| USB-C serial | debug channel and `text_in` | v0 |
| 2" 320×240 IPS (ILI9342C) | face: state stub → full animated face | v0 stub, v2 full |
| PSRAM 8 MB | audio buffers, sprite framebuffer, JPEG frames | v1–v3 |
| Speaker + AW88298 | streaming TTS playback | v1 |
| Dual mics (ES7210) | 16 kHz PCM16 capture; direction, robust VAD, AEC | v1 mono, v2 direction, v3 AEC |
| Touch (FT6336U) | backup PTT, touch reactions, skin carousel | v1 PTT, v2 reactions |
| Proximity (LTR-553) | wake on an approaching hand, gaze toward it | v2 |
| IMU (BMI270) | tilt/shake/carry/fall reactions | v2 |
| Camera (GC0308) | "look and tell", presence, background emotion read | v3 |
| Ambient light (LTR-553) | auto-brightness of the face | later |
| RTC (BM8563) | time without network | later (with v4 memory) |
| microSD | skin pack cache, local logs | later |
| AXP2101 + 500 mAh | power; charge level as "tiredness" | v0 power, indication later |
| Bottom3 halo (10× WS2812) | `accent_color` → emotion halo | v5 (optional) |
| BLE 5 | not used (WiFi-first) | out of plan |

## Contracts

These cross the tier boundary and are pinned by contract tests. Changing one changes its test in the same commit.

### WS device↔server

**The transport is `ws://` today, not `wss://`.** Everything else in this document says WSS, and that remains the intent — but the server runs uvicorn with no `ssl_keyfile`/`ssl_certfile`, so there is no TLS anywhere in the product as built. The firmware connects in plaintext over the LAN and keeps the scheme in `firmware/src/config.h`, so closing the gap is that line plus a certificate and a CA bundle rather than a rewrite. Recorded here rather than left implied: a specification that promises a transport the product does not offer is worse than one that says where it has got to.

Text frames are JSON objects with a `type`. Binary frames carry raw payloads with **no JSON envelope** — their meaning comes from direction plus connection state.

**device → server:** `hello{device_id, proto_ver, audio_fmt, caps}` · `listen_start` · `audio`(binary PCM16) · `listen_stop` · `text_in{text}` · `event{type: touch|motion|proximity, kind, meta?}` · `image_in{reason, w, h}` + `image`(binary JPEG) · `ping`

**server → device:** `asr_partial{text}` · `asr{text}` · `reply{text, final}` · `emotion{...EmotionFrame}` · `tts_audio`(binary PCM16) · `tts_end` · `config_updated{face_set?, brightness?, presence?}` · `error{code, msg}` · `restart` · `pong`

Audio is **PCM16, 16 kHz, mono** in both directions (the device down-mixes its two channels before sending — the uplink is never stereo). `hello.audio_fmt` spells that as `format/rate/channels` — **`pcm16/16000/1`** — and `protocol.py` owns the constant the ASR and TTS adapters must agree with. Images are **JPEG** from the camera, announced by the `image_in` control frame that immediately precedes the binary frame.

`hello.proto_ver` is the wire version, currently **1**. There is no compatibility window: a device announcing any other version is rejected with `proto_unsupported` and the socket closes. One server serves one household's devices and they are flashed together.

A text frame is capped at **64 KiB** and refused before it is parsed — control frames are small by construction, and the server binds every interface with no authentication, so an unbounded frame is an unbounded allocation from anything that can reach the port. Bulk payloads never travel this way: audio and images are binary frames, each carrying its own limit from the version that introduces it.

`hello.caps` is the capability set (§Hardware variants) — `touch`, `camera`, `dual_mic`, `halo`, `buttons` — and the server tailors what it sends to it.

### EmotionFrame

The entire face channel, server→device, one object:

```json
{ "emotion": "joy", "intensity": 0.8, "gaze": {"x": -0.4, "y": 0.0},
  "accent_color": "#5FFFC4", "speaking": true, "ttl_ms": 6000 }
```

- `emotion` — the fixed enum: `neutral · calm · joy · thinking · surprised · sad · error`. Required.
- `intensity` — 0..1, scales expressiveness: smile depth, eye opening, idle amplitude, halo brightness. Required.
- `gaze` — continuous `{x, y}` in −1..1. Driven by the server (voice direction, the vision turn) and overridden locally by a reflex (a hand approaching, a touch). Optional; **omitted means "no opinion"**, which is not the same instruction as centre once a device reflex can override one of them.
- `accent_color` — `#rrggbb`, and nothing else — the device draws in RGB565 and would have to reject anything it could not convert. The skin's element colour and, from v5, the halo. Optional; omitted means "use the recipe colour".
- `speaking` — whether lip-sync is permitted. Optional, default false. **A permission, not a duration:** the device is seconds ahead of the server's idea of when speech ends, because it is still draining audio the server finished sending, so what stops the mouth is the device's own playback state (v2.1.2).
- `ttl_ms` — with no new frame after this, the renderer relaxes to `neutral`. Optional, default 8000. A liveness guarantee rather than a schedule: it is what stops a face holding `thinking` forever if the connection drops between the model call starting and its answer arriving.

**Every field is coerced, on both sides, and never refused.** `emotion` outside the enum → `neutral`;
`intensity` clamped, and a non-number → 0.5; a gaze axis clamped, a malformed gaze dropped; a
malformed `accent_color` dropped; a non-positive `ttl_ms` → the default. This is the one frame whose
decoder does not raise: a face is not worth dropping a connection over, and the two tiers are
separately releasable, so neither may assume the other has already sanitised what it sends. Each
coercion logs once at debug — a model that keeps reporting `"happy"` is a prompt bug, and that line
is the only way to find it. The encoder omits any optional field still at its default, so a frame on
every state change does not bury the field that actually moved.

**The server decides `emotion`; the device never infers one.** Turn states are expressed through the face — `listening`, `thinking`, `replying` map to frames (`calm`, `thinking`, and the reply's own emotion), not to text labels on the screen. The **audio level is not part of this object**: lip-sync is a local `setAudioLevel(0..1)` derived from the playback buffer.

### event{}

The second level of the reaction model. The reflex already fired locally; this tells the character it happened.

```json
{ "type": "event",
  "event": { "type": "touch", "kind": "stroke", "meta": {"zone": "cheek", "count": 3} } }
```

**The event's own type is nested, and the nesting is not decoration.** The envelope's `type` names
the *message* — a rule every frame in this protocol follows and `decode_envelope` depends on — so an
event that spread its fields into the envelope would overwrite it, and the frame would decode as an
unknown message called `touch`. Earlier revisions of this section showed only the inner object; that
was the object being described, not the frame on the wire.

`kind` is a small enum per type — touch: `tap · multi_tap · stroke · poke_eye · long_press`; motion: `tilt · shake · picked_up · upside_down · free_fall`; proximity: `approach · leave`; voice: `direction`. The server may answer with an `emotion` frame, a spoken line, or nothing at all.

`voice`/`direction` (v2.5) is the one type that is not a thing that happened *to* the device: it carries `meta.x` in [-1, +1], where the two microphones heard a speaker. It rides this channel because it is the same kind of claim as the other three — something the device sensed and the server could not have known — and it is answered with **neither** a face nor a line. The server remembers it per connection and attaches it as `gaze` to the next frame it had a reason to send; a frame of its own per update would put a face change on the wire every time someone shifted in their chair.

### Error codes

Enumerated, never free text: `wifi_lost · server_unreachable · proto_unsupported · unauthorized · rate_limited · asr_failed · llm_timeout · llm_failed · tts_failed · vision_failed · bad_frame · internal`.

`bad_frame` and `internal` split by **whose fault it is**: `bad_frame` means the device sent something the server cannot parse — malformed JSON, an unknown message type, or one this version does not implement — and `internal` means the server itself broke. The distinction matters at the far end: from v0.4 the device renders the error face for whatever code it is sent, and blaming the server for a device's own frame would send the wrong face and the wrong retry behaviour.

## Turn lifecycle

Half-duplex until v3.4 (barge-in needs AEC).

```
VAD speech start → listen_start → audio(bin) chunks streaming while you speak
  → recogniser endpointing → asr   (the utterance ends here)
       └ device's local end-pause → listen_stop   (backstop only)
  → asr_partial* / asr → LLM deltas → reply deltas + emotion frame
  → phrase boundary → TTS chunks → tts_audio(bin)… → tts_end → idle
```

**The recogniser ends the utterance; the device's end-pause is the backstop** (v1.4). Both ends
hear the same silence, and the recogniser calls it first — it is endpointing at ~500 ms while the
device is still waiting out a longer, deliberately forgiving pause. Acting on the recogniser's
decision starts the turn earlier by that margin on every utterance, and it costs nothing: the
transcript is already in hand.

Two consequences follow, and both are pinned by `tests/contract/test_turn_lifecycle.py`:

- **A `listen_stop` for an already-settled utterance is idempotent, not an error.** The device is
  not wrong — its end-pause simply elapsed after the recogniser had decided. Treating it as a
  protocol violation would put a fault on the screen for every hands-free turn.
- **Binary frames arriving after the server settled are dropped quietly.** The device streams
  continuously and stops when it sees `asr`; the frames captured in between belong to an utterance
  already being answered.

The server notices a settled transcript on the **next** audio frame, which at 20 ms frames makes
the decision at most one frame late. An utterance that settles on its final frame has no next one
and is closed by `listen_stop` through the same path v1.3 used — which is also what happens when
the recogniser never endpoints at all.

Text path (v0, serial/debug): `text_in` → LLM → `reply`. Vision path (v3): `image_in` + JPEG ride into the same LLM call as multimodal input.

### Streaming is the architecture, not an optimisation

Every stage streams, and each stage starts on the *first* output of the one before it. Nothing in this pipeline waits for a stage to finish:

- **Capture streams.** The device sends ~20–40 ms `audio` chunks *while the person is speaking*, never one blob after they stop.
- **Recognition streams over a WebSocket.** Deepgram receives audio live with `interim_results` on and **server-side endpointing** (~500 ms silence) plus an `utterance_end_ms` (~1000 ms) backstop. Because recognition happens *during* speech, the transcript is already in hand when the utterance ends and the ASR leg costs almost nothing — batch recognition instead pays for the whole utterance *after* it ends, which is most of a latency budget. An un-punctuated `speech_final` (a breathing pause) is **held** for the continuation or the backstop, so the character never answers half a phrase.

  **An empty final does not discard a good draft.** Deepgram regularly publishes a sentence as an interim and then closes the span with an empty `is_final` — observed on audio the same vendor transcribes perfectly in batch mode. It happens whenever the window closes soon after the speech ends, so the vendor never receives the trailing silence its own endpointing wants and finalises on the stream closing instead: the normal case for a device with its own VAD. `UtteranceTracker` therefore keeps the most confident interim of the current span and falls back to it when nothing settles. A real final always wins; the draft is a last resort, never a competitor. `ASRChunk` carries `confidence` for this one decision, because choosing the surviving draft by *length* would prefer a long mishearing over a short correct one.
- **Generation streams.** Gemini token deltas leave as individual `reply` frames as they arrive; the reply is never accumulated and sent whole.
- **Synthesis streams per phrase.** A pure **`PhraseSplitter`** consumes the deltas and emits a phrase at each sentence (`. ! ? …`) or clause (`;`) boundary — but only when followed by whitespace, so `3.5` and a boundary at the end of the buffer don't split early — skipping Ukrainian abbreviations (`напр.`, `т.д.`, `вул.` …), force-flushing a runaway clause at `max_chars` on a word boundary, and flushing the tail for short replies. Each completed phrase is synthesized immediately through ElevenLabs' stream endpoint at `pcm_16000` — the device's exact playback format, so there is no decode step.
- **Playback streams.** The device switches the shared bus mic→speaker on the first chunk, plays each `tts_audio` frame as it arrives, and drains on `tts_end`. The buffer is **lossless — consume exactly what plays**; a truncating callback garbles the voice. The same buffer feeds the lip-sync envelope, so the mouth is driven by audio that is still arriving.

The observable consequence, and the thing to test: **the first audio leaves before the reply is finished** — `tts_audio` frames interleave with `reply` deltas rather than following them in two bursts.

### Budgets and abort semantics

Each stage has its own budget, awaited on the stage's *first* output so a stalled provider fails fast while a slow-but-flowing one is never cut off mid-reply:

| Stage | Budget | On breach |
|---|---|---|
| ASR (final transcript) | ~5 s | `asr_failed` |
| LLM (first token) | ~8 s | `llm_timeout` |
| TTS (first audio of a phrase) | ~3 s | `tts_failed` |
| End to end (`listen_stop` → first audio) | **< ~1.5 s target** | measured, logged per leg |

A breach or provider failure aborts the turn to an enumerated `error{code}`, returns the device to `idle`, and **rolls the user message back out of history** so the next turn starts consistent. The session stays connected. Silence that transcribes to nothing ends the turn cleanly with `tts_end` — not an error.

From v3.4, audio frames carry the turn they belong to, so a barge-in that cancels a turn can drop late chunks from the abandoned one instead of playing them. Barge-in **pauses and queues rather than discarding**: the in-flight phrase is requeued whole and replays intact, and a watchdog resumes playback so silence can never leave the character permanently muted.

**Server-initiated frames.** The emotion engine may push an `emotion` frame outside a turn (a state change, a reaction to an `event{}`, a mirroring of the user's read emotion). The device plays whatever arrives whenever it arrives.

## Device states

`boot → wifi_connecting → idle → listening → thinking → replying → idle`, with `offline` (no WiFi/server) and `error` reachable from anywhere. The state drives the face; the device holds no persona logic, and transitions come from local input plus server messages. What each state actually looks like — face plus chrome, including the boot, not-configured, offline, fault and updating screens — is [features/DEVICE_UI.md](features/DEVICE_UI.md) §Screens.

## The face

### Renderer ladder

One `IFaceRenderer` interface across every tier, so improving the look is a renderer swap, not a rewrite:

```cpp
class IFaceRenderer {
  virtual bool begin();                       // load the skin pack into PSRAM; false if it could not
  virtual void show(const EmotionFrame&);     // set target expression (crossfade ~150–250 ms)
  virtual void show(DeviceState);             // the states the server cannot know — see below
  virtual void setAudioLevel(float lvl);      // 0..1 while speaking — lip-sync
  virtual void tick(uint32_t now_ms);         // idle loop: blink, breathe, gaze drift, blend, ttl
};
```

**Two `show` overloads, and the split is the authority rule rather than a transitional convenience.**
`boot`, `wifi_connecting`, `offline` and a local fault are facts about the link — in the `offline`
case, by definition facts about the server being unreachable — so the device must have a face of its
own for them or it would have none. The *turn* states are the server's: from v2.2 `listening`,
`thinking` and `replying` arrive as `emotion{}` frames and the device no longer infers an expression
from its own state machine.

**A frame stands for its `ttl_ms`, then the renderer relaxes to `neutral` and the idle loop
resumes.** Expiry is an edge, not a level: the relax is a one-off re-target, and a condition that
stayed true would re-target on every frame afterwards so the crossfade would never finish.

**`speaking` is a permission, and the device's own playback state is the fact.** The mouth runs on
both. The server's idea of when a reply ends runs seconds ahead of the speaker — the device is still
draining audio the server finished sending — and taking the flag literally froze the mouth
mid-reply in v2.1. Believing only the speaker would lip-sync a loopback recording or a chime.

1. **Stub (v0.4)** — the state expressed as a static face. Proves the channel end to end.
2. **Procedural Stack-chan (v2.1)** — layered M5GFX sprites drawn from primitives; no assets.
3. **Spirit sprite skins (v2.6)** — `ghost`, `flame`, `jelly`, `cloud`: the same layer scheme filled with authored art plus one "element" behaviour each (flame colour, cloud weather, jellyfish glow, ghost blush/tear).

### The viseme ladder

The mouth's opening while the device speaks is chosen from a **table**, not computed continuously —
`firmware/src/pure/lipsync.h` is the whole definition, and adding or removing a shape is adding or
removing a row.

Two shapes, not one, per rung: how far the mouth **opens** and how **wide** it is. A mouth that only
changes height reads as a jaw hinging; each rung therefore carries a spread and a pursed width, and
which one is used alternates each time the mouth shuts. No phonetics and no timer — the gaps between
syllables are already in the signal.

The signal is a **windowed RMS envelope** of the audio being played (`pure/envelope.h`), with an
immediate rise and an eased fall. Not a peak: a peak over one 32 ms playback chunk sits near full
scale through almost all of continuous speech, and a ladder fed from it holds one shape. The input
level *meter* keeps its peak, because a meter answers a different question — see the note in
`pure/level.h`. **Two consumers, two measures.**

The thresholds are calibrated against that envelope's measured distribution, and the property the
tests assert is not the numbers but what they produce: **every rung is reached by real speech and
none dominates.** A ladder is right when every shape is used; the numbers are how, and they become
wrong the moment the envelope changes.

Roadmap §v2.3 says four bands. The table has five, and it is the table that is authoritative — the
count is data, arrived at by measuring, and the roadmap's number was an estimate made before there
was anything to measure.

### Layer model and recipes

The face is composed from a small layer bank at render time, never stored as one image per emotion: background/glow → base → eyes → brows → mouth → element/overlay FX. An **emotion is a recipe** over that bank (which eye shape, which mouth, tilt, colour, idle amplitude), scaled by `intensity`. N emotions cost a small bank, not N faces.

`face-prototype.html` (in this directory) is the executable reference for the whole grammar: `renderFace(skin, frame)` is pure, the five skins share `sEyes`/`sMouth` with per-skin ink and coordinates, and the JSON under the screen is exactly what the server sends. Port that structure; do not invent a second one.

### Skins

All five faces are **skins over the same `EmotionFrame`**. A skin is a manifest entry plus shapes and a palette — if a skin needs renderer logic, the design is wrong. Switching happens two ways: `config_updated{face_set}` from the server, or the **carousel** on the device — a hold that passes 1.2 s without speech, specified in [features/DEVICE_UI.md](features/DEVICE_UI.md) §Input. The active skin is reported back in the next `hello`.

### Lip-sync

Local, always. The device computes a short-window RMS envelope of the audio it is playing and maps it to mouth openness:

```
level < 0.10 → closed    < 0.35 → small    < 0.65 → mid    else → wide
```

~15–20 FPS over the layered sprites. **No server data is involved** — the server only sets `speaking`.

### Chrome around the face

Indicators, notifications and input affordances are specified in **[features/DEVICE_UI.md](features/DEVICE_UI.md)** and prototyped in [device-ui-prototype.html](device-ui-prototype.html). The rules that matter architecturally: the face expresses *state* and chrome expresses only *facts a face cannot say*; chrome lives in the outer 28 px band and never overlaps the central 264×184; everything fades ~3 s after it settles except an unresolved fault, a muted mic and the **camera lens indicator, which is lit whenever the camera is powered and has no way to hide** — the visible half of the privacy rule. Chrome is driven by device-local facts and `error{code}`, never by `EmotionFrame`.

### Gaze

`gaze` arrives from the server (voice direction from v2.5, the vision turn in v3) and is overridden locally by reflexes: the proximity sensor pulls the gaze toward an approaching hand; a touch pulls it to the touched zone; the IMU rolls the eyes against a tilt to keep the horizon.

**The order of authority, from v2.4 where the field acquired its first consumer and v2.5 where it
acquired its first producer:**

1. **a local reflex** — a hand at the proximity sensor, a touched zone — while it is active;
2. **the local voice direction** (v2.5), while the estimate is `present`;
3. **the server's last `gaze`**;
4. **the idle loop's drift**, which composes with any of them because it is a wander, not a look.

A hand outranks a voice because it is a nearer and more specific claim on attention: someone
reaching toward the device is addressing it, someone talking across the room may not be. And the
voice estimate outranks the server's because it is the same fact without the round trip — the device
reports the direction, and the frame that comes back is a confirmation of something it already did.

**`present` is a third state, not a centred gaze.** A speaker directly in front of the device
produces a balance near zero, which is the absence of evidence rather than an instruction to stare
straight ahead. Reporting it as centre would override the idle drift and hold the eyes rigidly
forward — which is why both `DirectionEstimate.present` on the device and `Gaze | None` on the wire
distinguish "no opinion" from "look ahead". The reflex wins for a reason that is not about
layering — the device can see a hand and the server cannot, being one round trip and a model call
away from knowing. A gaze that waited for permission would always be looking at where the hand had
been. When the reflex releases, the face returns to the server's gaze rather than to centre: the
server's instruction did not stop being true while a hand was in the way.

### `config_updated{face_set}` — the one setting the server owns

The only server→device frame that is not about a turn, and the only place the server says what the
device should **look like** rather than what it should express. `face_set` is one of the five
declared names — `stackchan · ghost · flame · jelly · cloud` — held on the server as `FACE_SETS` and
on the device as `pure/skins.h`, with a contract test checking the two agree.

**Refused rather than coerced, on both sides and in both directions.** That is the opposite of
`emotion{}`, and the contrast is the reasoning: a bad emotion becomes `neutral` and is rendered,
because a face is not worth dropping a connection over. A bad `face_set` is a disagreement about
*what faces exist* — the two sides were built from different vocabularies — and accepting one would
leave the server believing the device wears a skin it has never heard of. An unknown name that is
quietly ignored is indistinguishable from a switch that worked.

The device reports the face it is wearing on **`hello`**, not in a frame of its own: the question is
only ever asked at the start of a connection, and a device switched to the ghost and then
reconnecting says so without the server having to remember across a socket it may never see again.
The field is omitted rather than sent as null when a device has no skins, which keeps a pre-v2.6
firmware valid — no opinion is not the same as claiming to be faceless.

**A skin is data, and the renderer holds none of its own** (v2.6). `pure/skin.h` is the schema —
anchors, colours, a body and an element — derived from `face-prototype.html`'s own argument lists
rather than invented beside them. The procedural face is a manifest like the four spirits, which is
what makes *"adding a skin requires no renderer code change"* testable rather than aspirational.

**Packs are linked in, not files — and that is a deliberate narrowing of §v2.6, stated rather than
quietly done.** The roadmap asks for *"one directory per skin under `assets/`"* and *"load a pack
into PSRAM at `begin()`"*. What ships is the whole of the validating load path — `loadSkin` takes a
candidate manifest, validates it, and substitutes the fallback face with a named reason — plus the
art itself as one binary linked by `.incbin`. A filesystem would add a failure surface (an absent
card, an unformatted partition, a half-written file) for no difference the person in front of the
device could see, and every one of those failures resolves to the same fallback this path already
implements and tests. When a later phase wants user-supplied skins, the seam it needs is here.

**Nothing is loaded into PSRAM either**, and that is a second, smaller departure from §v2.6 worth
naming. Flash is memory-mapped on the ESP32-S3, so a body is read where it lies: 876 KB of art costs
876 KB of flash and no RAM at all, against a sprite that already holds 150 KB of PSRAM. Decoding
happens on the workstation (`tools/skin_assets.py`), so the firmware links no PNG decoder and spends
nothing at boot.

**The art is three encodings, chosen by the question the renderer asks of the pixels.** An opaque
body is RGB565 and is copied. A tinted body — flame, jelly, cloud, whose element *is* their colour —
is 8-bit luminance multiplied by the emotion's tint. A feature is 8-bit alpha, because only the
alpha carries shape and the ink belongs to the skin: that is what lets one set of nineteen images
serve all five faces, and it is `face-prototype.html`'s own arrangement made data.

That last point is also why **the sprite is 16-bit rather than 8**. Eight was chosen for the frame
budget and measured, back when a face was a background, a glow and one ink. Drawn art cannot live in
a palette — not because 256 colours is too few, but because **an index cannot be blended**, and every
feature is a mask mixed into what is under it.

**A face is never absent**, and that is the definition of the feature rather than an error path. A
device that cannot draw a face has no way to say so — not that a pack is broken, not that the WiFi
is down, not that it is listening. Every skin answers for itself in the boot log, healthy or not,
because v2.4 established that a subsystem which says nothing when it is well cannot be told apart
from one that says nothing because it never ran.

The honest boundary of that claim: `SkinBody` and `SkinElement` are **closed enums**, and drawing
them is the renderer's vocabulary. A skin combining an existing body, an existing element, its own
anchors and its own palette costs no renderer code. A skin needing a shape nobody has drawn costs
one `case` — and the enums are closed precisely so that this announces itself at the moment it is
written rather than after a renderer has quietly grown four special cases.

## Interaction — two levels

Every physical input is handled twice, and the two levels never block each other.

1. **Reflex (device, <100 ms).** Pure animation over the current state. It never interrupts speech or listening and never waits for the network. Tap → wink and a small smile; repeated taps → growing joy and blush; stroke → contented arc eyes; poke in the eye → surprise and a recoil; tilt → eyes roll against it; shake → dizzy spirals; picked up → wide alert eyes; upside down → the face flips and frowns; free fall → full-screen fright.
2. **Report (`event{}` → server).** The same input is sent up, and the character may answer with an emotion change or a spoken line (prolonged shaking → offence and a remark). If the server is unreachable, level 1 still works — the face never depends on the network to feel alive.

The hold is layered rather than split: a press is **PTT** from the first ~120 ms, and only a hold that passes **1.2 s with no speech detected** converts into the **skin carousel** (announced by the carousel dots appearing). Control gestures — carousel, status sheet, mute — are local UI and are deliberately *not* reported as `event{}`: the character has no opinion about your volume. The full input map, per board, is in [features/DEVICE_UI.md](features/DEVICE_UI.md).

## Audio pipeline

- **Capture.** ES7210, 16 kHz PCM16. The device processes stereo locally and always sends **mono** upstream.
- **Active listening (v1.4).** The default mode: the mic is always open, a VAD endpointer marks speech start and the end-of-utterance pause. Touch-and-hold remains as the backup PTT. While the device speaks, listening is paused (half-duplex) until v3.4.
- **Dual-mic rollout.** v1: one channel, simplest thing that works. v2.5: direction from the inter-mic time/level difference → `gaze`, plus channel selection/summing for a cleaner ASR signal, and the start of dual-channel VAD robustness. v3.4: the esp-sr AFE (AEC + noise suppression + VAD) using the playback reference channel, which makes listening full-duplex and enables **barge-in**.
- **Playback.** Streaming `tts_audio` frames into a PSRAM ring buffer feeding AW88298; the same buffer feeds the lip-sync envelope.

## Vision

Three tiers, all v3, and all subject to the privacy rule below.

1. **Look and tell (v3.1).** On demand ("what do you see?") the device captures a JPEG, announces it with `image_in` and sends it; the server attaches it to that turn's Gemini call as multimodal input and answers by voice.
2. **Presence (v3.2).** Cheap local motion/brightness detection plus the proximity sensor wakes the face and greets someone who sits down. No identity, no recognition.
3. **Background emotion read (v3.3).** A **separate channel and a separate Gemini call**, fully outside the turn pipeline: while presence mode is on, the device sends a small frame every few seconds; a background task asks "what emotion is the person showing?" (enum + intensity) and puts the answer in two places — a **mood line** in the next turn's system prompt, and optionally an **immediate mirror** on the face via an `EmotionFrame`. **It never blocks or delays a turn**: if the background call is late, the turn goes without it. Wake-on-face falls out of the same channel.

**Privacy.** Frames are processed in memory and never written to disk. The camera is live only during an explicit turn or in presence mode, and presence mode is opt-in in the role/config.

## The mind (v4)

Deliberately small, entirely server-side, no console:

- **Canon** — one authored text file describing the RoboFace character (a separate character, not Pyramid's or Lumi's), assembled into the system prompt.
- **Memory** — session history: the last **40 messages**; plus **facts about the interlocutor**: up to **500 facts of 2 lines** each, extracted by the model from the conversation, stored in SQLite and mixed into the system prompt.
- **One tool** — web search via Gemini's built-in **`google_search` grounding**: the model searches and answers with sources, so no separate search API and no tool-loop machinery.
- **World context** — a prompt block the server refreshes itself: date/weekday/time-of-day from the server clock, location from config, weather (one call, cached an hour) and ~10 lines of headlines (RSS or the same grounding, cached a few hours).

MCP, the role model and temperament are deliberately absent and are not "coming in v4.5" — they are out of this project's scope.

## Model policy — Gemini only

**Chat and vision run on Gemini 2.5 Flash, with thinking disabled (`thinkingBudget: 0`), and on nothing else.** The rationale: the lowest time-to-first-token and cost for a short conversational character, multimodal input that covers both the vision turn and the background emotion read, and `google_search` grounding that removes the need for a separate tool stack. There is one real `LLMProvider` implementation plus a mock; a second chat vendor is a non-goal, not a backlog item.

Speech is not chat, and each has its own seam: **Deepgram** for ASR, **ElevenLabs** for TTS. Swapping either is a provider change, not an architecture change.

## Providers and seams

Five seams carry the whole system. Each is pinned by a contract test, and each has a mock used by default in tests:

| Seam | Where | What it hides |
|---|---|---|
| WS protocol | `server/…/protocol.py` + `firmware/…/ws_protocol.h` | message set, binary-frame rules, error codes |
| `EmotionFrame` | protocol + `IFaceRenderer` | the entire face channel |
| `LLMProvider` | `server/…/providers/` | Gemini (the only real impl) — streams reply deltas **and the model's emotion report** |
| `ASRProvider` / `TTSProvider` | `server/…/providers/` | Deepgram (WS, interims + endpointing) / ElevenLabs (stream endpoint, `pcm_16000`) — both yield chunks as they arrive |
| `IFaceRenderer` + skin manifest | `firmware/` + `assets/` | which skin draws the frame |

The two streaming provider seams share one shape, and it is deliberate:

```python
class LLMProvider(Protocol):
    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[LLMEvent]: ...

LLMEvent = ReplyText | ModelReport      # from v2.2: a reply is not only text

class TTSProvider(Protocol):          # from v1.1
    def synthesize(self, text: str) -> AsyncIterator[bytes]: ...   # PCM16, 16 kHz, mono

class ASRProvider(Protocol):          # from v1.3
    def open(self) -> ASRSession: ...                              # a session, not a call
```

**`LLMProvider` yields a union because the model reports its own emotion alongside the reply, and
the two arrive interleaved on one connection (v2.2).** A separate call asking "how do you feel about
what you just said?" would cost a round trip on every turn — and by then the device has already
spoken the answer with the wrong face. The response schema declares `emotion` and `intensity`
**before** `reply`, so the report arrives in the model's first chunk and the expression changes as
the device begins to speak; the JSON is read incrementally (`jsonstream.py`) rather than collected,
because collecting it would cost the entire generation time before the first word. **The report is
untrusted**: `ModelReport`'s fields are typed `object`, and `EmotionFrame.from_model` is where they
become renderable. A stream may carry no report at all — that is not a failure, and the emotion
engine falls back to `neutral`.

**`ASRProvider` is a session because recognition must run *during* speech.** Audio is pushed in
while transcripts are read out, so at the moment the person stops the transcript already exists. A
seam shaped `transcribe(audio) -> str` could only start once the audio was complete, and would pay
about 1.4 s at the worst possible moment — after the speaker has finished and is waiting. That one
choice is most of v1's latency budget.

`is_final` on an `ASRChunk` is the vendor saying it will not revise *that span*. It is **not** a
claim that the person has stopped talking; that judgement belongs to `utterance.py`, and conflating
the two makes the character answer someone who paused for breath.

**Neither is `async def`.** The method returns the iterator, so the caller holds it before awaiting
anything and can put a budget on the *first* output alone — the LLM's first token, TTS's first
audio chunk. An `async def` returning an iterator would move that choice into the implementation,
which is exactly where it must not live.

`TTSProvider` yields the device's playback format directly (`pcm_16000` from ElevenLabs), so nothing
decodes on the server or on the device. An implementation that accumulated a whole phrase before
yielding would satisfy the signature and defeat v1.1 entirely, so the contract test asserts the
streaming shape rather than only the types.

### The transport seam, and audio on it (v1.1)

`Transport` is the socket reduced to what the router needs. Until v1.1 it could carry only text:

```python
class Transport(Protocol):
    async def send(self, data: str) -> None: ...
    async def send_bytes(self, data: bytes) -> None: ...    # v1.1
    async def receive(self) -> str | bytes: ...
    async def close(self, code: int = WS_CLOSE_NORMAL) -> None: ...
```

`tts_audio`, `tts_end` and `tts_failed` were declared in v0.1 and mirrored in the firmware from
v0.3, and `server_binary_meaning(SPEAKING) → TTS_AUDIO` already gave an unlabelled binary frame its
meaning — but there was no way to put bytes on the wire at all. That gap, not the vocabulary, is
what v1.1 opened.

`send_bytes` is a separate method rather than a `str | bytes` parameter because the two are
different frame kinds on the wire, and a caller that got the union wrong would send a JSON string
where the device expects PCM.

**A turn emits events, not strings.** `Responder.respond` yields `ReplyDelta | AudioChunk`
(`turn.py`), because text and audio are *interleaved* — a phrase's audio leaves while later words
are still being generated. Two separate iterators would have to be merged by the consumer, and the
merge is exactly where the interleaving would be lost. The router translates: `ReplyDelta` becomes a
`reply` text frame, `AudioChunk` becomes an unlabelled binary one. The orchestrator does not know
how a chunk is framed, only that it exists and is ready now.

**The speaking window opens on the first chunk and closes on every exit path.** `BinaryPhase.SPEAKING`
is set before the first `tts_audio` — not at the start of the turn — so a device seeing binary
outside the window it was told about is right to treat it as a protocol violation. `tts_end` closes
it before the turn ends, and also before an `error`: the device switches its shared I2S bus to the
speaker when audio starts and back on `tts_end`, so a turn that aborted mid-audio without closing
the window would leave the bus on the speaker and the device unable to listen — a fault whose
symptom appears one turn later, in a subsystem that is working correctly.

### The listening window (v1.2)

Audio travels device → server as **unlabelled binary frames**, exactly as `tts_audio` travels the
other way, and what gives them meaning is the connection's phase:

```
listen_start          → BinaryPhase.LISTENING       (device_binary_meaning → AUDIO)
  <binary> × N        → appended to the utterance
listen_stop           → assembled, released, phase back to IDLE
```

`listen_start`, `audio` and `listen_stop` were declared in v0.1; v1.2 added the typed frames, the
router state and the cap. A binary frame **outside** the window is a `bad_frame`, and that refusal
is what makes the unlabelled framing safe — without it, an unlabelled payload would have a default
meaning, which is the thing the phase model exists to avoid.

**The utterance is capped in bytes, not seconds** (`MAX_UTTERANCE_BYTES`, 30 s at `pcm16/16000/1`).
Bytes are a property of what arrived; seconds would have to be trusted across a network. The cap is
checked **before** storing, so the memory it exists to protect is never taken, and exceeding it ends
the window with an enumerated error rather than truncating — someone who talked too long should be
told, not silently half-heard. The connection survives, the same way a failed turn keeps its session.

`listen_start` while already listening, and `listen_stop` while not, are **protocol errors rather
than no-ops**: they mean the device and the server disagree about state, and a silently restarted
window would lose whatever preceded it.

An **empty utterance is valid** — a window opened and closed with nothing between. v1.2's
press-and-hold can produce one, and v1.3's ASR must be able to see zero bytes rather than a fault.

## Data model

Nothing is persisted before v4 beyond a device record and config. From v4, SQLite behind a thin repository:

- `Device{device_id, name, caps, last_seen, face_set}`
- `SessionMessage{session_id, role, text, emotion, ts}` — the rolling 40-message window. **The window itself is enforced in memory from v0.2**, because an unbounded history costs more per turn than the last one did and eventually meets the model's context limit; what v4 adds is *persistence*, not the bound.
- `Fact{id, text (≤2 lines), confidence, source_ts}` — up to 500
- `WorldSnapshot{kind, payload, fetched_at}` — the cached weather/news blocks

## Configuration and secrets

Server configuration and keys live in `server/.env` (gitignored); `server/.env.example` carries the same shape with the secrets blank and is committed. **The firmware never holds a model key** — it holds only the WiFi credentials and the server URL, in a gitignored `firmware/src/config.h` generated from `config.example.h`.

| Variable | Purpose | From |
|---|---|---|
| `GEMINI_API_KEY` | the only chat/vision vendor | v0.2 |
| `GEMINI_MODEL` = `gemini-2.5-flash` | chat and the multimodal vision turn | v0.2 |
| `GEMINI_THINKING_BUDGET` = `0` | thinking off — lowest time-to-first-token | v0.2 |
| `GEMINI_EMOTION_MODEL` | the background emotion read, a separate call | v3.3 |
| `GEMINI_GROUNDING` / `GEMINI_GROUNDING_MODEL` | `google_search` grounding, the one tool | v4.3 |
| `ELEVENLABS_API_KEY` | TTS | v1.1 |
| `ELEVENLABS_VOICE_ID` | the voice RoboFace speaks in | v1.1 |
| `ELEVENLABS_MODEL` = `eleven_turbo_v2_5` | multilingual, handles Ukrainian | v1.1 |
| `ELEVENLABS_OUTPUT_FORMAT` = `pcm_16000` | matches the device's playback format exactly | v1.1 |
| `DEEPGRAM_API_KEY` | ASR | v1.3 |
| `DEEPGRAM_MODEL` = `nova-2`, `DEEPGRAM_LANGUAGE` = `uk` | recognition model and language | v1.3 |
| `DEEPGRAM_ENCODING` = `linear16`, `DEEPGRAM_SAMPLE_RATE` = `16000` | the uplink audio format | v1.3 |
| `DEEPGRAM_ENDPOINT_MS` | end-of-utterance pause | v1.3 |
| `ROBOFACE_WS_HOST` / `ROBOFACE_WS_PORT` = `8000` | the WSS bind (codegen's dashboard owns 8420) | v0.1 |
| `ROBOFACE_FACE_SET` = `stackchan` | the default skin | v0.4 |
| `ROBOFACE_CANON_PATH` | the character file | v4.1 |
| `ROBOFACE_LOG_LEVEL` | logging | v0.1 |
| `LOCATION`, `WEATHER_URL`, `NEWS_API_KEY` | world context; each source individually optional | v4.4 |

The audio format is fixed in three places at once — `ELEVENLABS_OUTPUT_FORMAT`, `DEEPGRAM_ENCODING`/`SAMPLE_RATE`, and the protocol's PCM16/16 kHz/mono constant. They must agree; the contract test asserts the protocol constant, and the provider adapters read theirs from here.

## Error handling and resilience

- Every failure maps to an enumerated `error.code`; the device renders the `error` emotion rather than a stack trace.
- Per-stage budgets: ASR, LLM and TTS each have a timeout; exceeding one fails that turn cleanly and returns the device to `idle`.
- WiFi/server loss → `offline`: the face keeps its idle loop and its reflexes; reconnection is exponential backoff with jitter.
- A background emotion read that fails or is late is dropped silently — it must never affect a turn.
- The renderer degrades: a missing skin pack falls back to the procedural Stack-chan face; an unknown `emotion` value falls back to `neutral`.

## Hardware variants and capability flags

One firmware, several boards. `hello.caps` tells the server what exists, and the firmware itself guards each subsystem:

- **Core S3 (v0–v5)** — the full set. With the optional **Bottom3** (v5): `halo`.
- **FIRE v2.7 (v6)** — classic ESP32, same 320×240 screen, **no touch, no camera, one mic**, built-in 10× SK6812. Degradation: three buttons replace touch (A = PTT/carousel, B/C = navigation), the dual-mic features are off and listening stays half-duplex, all of Vision is unavailable, the halo works immediately with the v5 logic. The face, the skins, lip-sync and IMU reactions work fully.

The `caps` flags also select the **input map**: `touch` activates the gesture table, `buttons` the FIRE's A/B/C table — both in [features/DEVICE_UI.md](features/DEVICE_UI.md) §Input.

Until v6 the firmware is written purely for the Core S3; the port is a one-off job inside that version, not a constraint on earlier ones.

## Firmware architecture

Two kinds of unit, the same split that made Pyramid's firmware testable:

1. **Pure logic** — header-only, Arduino-free, in `namespace roboface`: frame parsing and framing, the turn state machine, VAD, the lip-sync envelope, gaze math, the emotion recipe table, backoff. No `M5`/`WiFi` includes, so it compiles and is unit-tested on the host (`pio test -e native`).
2. **Glue** — `.h`/`.cpp` pairs in `namespace app`: `main` (wiring only), `net` (WiFi + reconnect supervisor), `ws` (the WSS client), `audio_io` (capture + playback), `face` (M5GFX sprite renderer + skins), `sensors` (touch, IMU, proximity), `camera`, `state`. Validated by compile plus on-device smoke checks in each phase's DoD.

Parsing, decisions and math never live behind an `M5` include; hardware access never lives in pure logic.

## Stack and repository layout

```
/firmware       # C++ / PlatformIO, M5Unified + M5GFX — Core S3 (FIRE from v6)
/server         # Python: FastAPI + websockets — protocol, router, orchestrator, providers, emotion engine
/assets         # face skin packs (stackchan/, ghost/, flame/, jelly/, cloud/) + manifest
/tests          # pytest: unit, contract, integration — fake device + mocked providers
/tools          # dev utilities — the terminal chat client that stands in for the device until v0.3
/specification  # MISSION.md, ARCHITECTURE.md, ROADMAP.md, features/DEVICE_UI.md,
                #   face-prototype.html, device-ui-prototype.html,
                #   the original CONCEPT*.md, roadmap/implementation/
/codegen        # code-generation tracking + dashboard (never imported by the product)
.github/workflows/ci.yml   # lint + tests on every push/PR
```

`pyproject.toml` at the root sets `pythonpath = ["server"]` so tests import the server package; `codegen/` keeps its own `pyproject.toml` and is tested from inside its own directory.

## Testing and CI

Tests ship with each phase — they encode its DoD, and `main` stays green.

- **Unit** — pure logic on both sides: prompt assembly, history windowing, error mapping, emotion recipes, VAD, framing, the lip-sync envelope, gaze math.
- **Contract** — the five seams above. The WS message set, `EmotionFrame`, the provider interfaces and the skin manifest each have a test that must change when the contract does.
- **Fakes, not hardware or paid APIs** — a **fake device** speaks the WS protocol in tests, and **mock Gemini/Deepgram/ElevenLabs** return canned streams. CI never makes a paid call; a live call is opt-in and manual.
- **Integration** — a full turn end-to-end against the fakes: `text_in → reply` (v0), `audio → tts_end` (v1+), `image_in → reply` (v3), asserting state transitions and the error paths. These also **assert the streaming property**: `reply` deltas arrive as many frames, `tts_audio` interleaves with them rather than following in a second burst, and an aborted turn leaves history consistent.
- **Firmware** — host-testable logic under `pio test -e native`; hardware I/O is covered by the manual DoD checks in the roadmap.
- **CI** — `ruff` + `pytest` (and `mypy server`, strict) on every push/PR.

## Code-generation tracking

`codegen/` records how this repository is built by the SDLC skills in `.claude/skills/`: an append-only event log per run, hooks that observe tool use independently of what the skills claim, a reducer, and a dashboard on port 8420. It is **subject-independent** — it imports nothing from `server/` or `firmware/`, survives a reset of the generated tree, and its own suite runs from inside `codegen/`. See [codegen/README.md](../codegen/README.md).
