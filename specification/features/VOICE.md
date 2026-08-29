# The voice

RoboFace speaks with one voice, and it is a character rather than a setting.

## Who is speaking

**An inspired scientist who has just thought of something and cannot wait to tell you.**

Not a narrator, not an assistant. The register is a colleague leaning across a desk — someone who
finds the world interesting and assumes you will too.

- **Quick, not hurried.** An excited person speaks in bursts with pauses for thinking. A constant
  fast rate sounds anxious; no variation sounds bored. The variation *is* the character.
- **Warm, not soothing.** Warmth is interest in the person, not a calming register. A companion
  that sounds like a meditation app is one nobody talks to twice.
- **Confident enough to be wrong.** "Я не зовсім зрозумів" should sound curious, not apologetic.
- **Light, not deep.** Young and forward rather than resonant. A broadcast-weight voice makes the
  device furniture.

## The voice

`u9LAsbCKO1LaU4a2KYzO` — **"RoboFace"**, an original voice, not a preset. Designed from the description below
through ElevenLabs Voice Design and chosen by ear from three candidates.

The route there matters, because it is repeatable. Six stock male voices were auditioned first on
the same Ukrainian sentence; "Will" was the closest in character — young, light, unhurried, no
broadcast weight — but a preset is a voice somebody else's product also speaks with. So Will became
the *brief* rather than the answer, and the description below is that brief written out:

> A young man in his mid-twenties with a light, bright, forward voice — not deep, not resonant, no
> broadcast weight. Easy and relaxed, an unhurried optimist, but genuinely excited by ideas: he has
> just worked something out and wants to tell you about it. Speaks in quick bursts with real pauses
> for thinking, warm and friendly rather than soothing, curious rather than authoritative. Natural
> conversational energy, clear articulation, never theatrical.

Keep that text. Regenerating from it is how this voice is recovered if the id is ever lost, and it
is the only record of *why* the voice sounds the way it does.

## The settings, and why

`ELEVENLABS_VOICE_SETTINGS` in `server/.env`. Partial: what you name is overridden, the rest keep
these defaults.

| Setting | Value | Why |
|---|---|---|
| `speed` | 0.90 | An excited voice at full rate clips its own word endings, and **Ukrainian carries meaning in the endings** — a missing last syllable is not a matter of polish, it changes the case. Slower here buys articulation, not calm. |
| `stability` | 0.45 | High flattens every phrase into the same read; low lets pitch and pace move. Raised from 0.35 together with the speed: some of the swallowed endings were the pitch moving mid-word. Below ~0.25 the tone drifts between sentences. |
| `style` | 0.45 | Carries the speaker's own inflection through. Above ~0.7 it over-performs, costs latency, and rushes. |
| `similarity_boost` | 0.75 | Keeps timbre steady. Matters more here than usual — a reply is synthesised **phrase by phrase** as it streams, so drift would be audible inside one sentence. |
| `use_speaker_boost` | true | Clarity through a small desk speaker, the only way anyone hears this. |

## Two constraints that are not negotiable

**Every phrase is synthesised without knowing what follows it.** `PhraseSplitter` emits at each
clause boundary so speech starts while the model is still generating. A voice whose expressiveness
needed a whole paragraph would break up audibly; these settings were picked to sound right on a
single clause heard alone.

**`pcm_16000`** — exactly the device's playback format, so there is no decode step on a board with
no headroom for one. Fixed by `AUDIO_FMT` in `protocol.py`.

## Language

Ukrainian through `eleven_turbo_v2_5` — multilingual, and the fastest that is. The voice is an
English preset carried into Ukrainian with a slight accent: a deliberate trade, since a native
voice with the wrong temperament would be a worse companion than a well-chosen one with an accent.
Revisit if a Ukrainian voice with the right character appears.
