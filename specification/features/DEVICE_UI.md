# Device UI — chrome, input and notifications

The face is the interface. Everything in this document exists to stay **out of its way** and appear only when it carries information the face itself cannot. This is the on-device UI on the Core S3's 320×240 screen (and, from v6, the FIRE's); there is no web console in this project.

The mock-up of everything below: **[../device-ui-prototype.html](../device-ui-prototype.html)** — every screen, indicator, gesture and notification, interactive.

## Rules

1. **The face expresses state; chrome expresses facts.** `listening`, `thinking`, `replying` are read from the face — never written as a word on screen. Chrome carries only what a face cannot say: signal, charge, whether the camera is live, what a gesture just did.
2. **Nothing permanent but a problem.** Every indicator fades to invisible ~3 s after it settles. What stays visible is exactly: an unresolved fault, a live camera, and a muted microphone.
3. **One line, never a paragraph.** A notification is a glyph plus at most ~28 characters. Anything longer is something the character should *say*, not print.
4. **Never block the face.** Chrome lives in the outer 28 px band; the face keeps the central 264×184.
5. **Reflexes are the feedback.** A touch is acknowledged by the face reacting within 100 ms — no ripple, no flash, no click. UI feedback is only for things the face cannot show (a skin change, a mode change).
6. **The privacy indicator is not optional.** Whenever the camera is powered, the lens dot is lit. It has no fade timer and no way to hide it.

## Layout — the 320×240 screen

```
┌──────────────────────────────────────────────┐ 0,0
│ ◎ lens            face safe area      ▮ ▯ ⌁  │  status cluster (top-right, 12 px glyphs)
│  ┌────────────────────────────────────────┐  │
│  │                                        │  │
│  │            face 264 × 184              │  │  centred, never overlapped
│  │                                        │  │
│  └────────────────────────────────────────┘  │
│  ▁▂▃▅▃▂▁  level / toast / carousel band      │  bottom band, 28 px, one thing at a time
└──────────────────────────────────────────────┘ 320,240
```

The bottom band is **single-tenant**: the input-level meter, a toast and the skin carousel never share it. Priority when two want it at once: carousel > toast > level meter.

## Indicators

| Indicator | Where | Shown when | Hidden when |
|---|---|---|---|
| **Link** — three arcs | top-right | connecting (arcs cycle), degraded (one arc, amber), offline (crossed, amber) | connected and settled ~3 s |
| **Battery** — pill with fill | top-right | below 20 %, or while charging (fill animates) | above 20 % on battery |
| **Lens** — filled dot | top-**left** | **always, while the camera is powered** — capture, presence, or the background read | camera unpowered |
| **Mute** — mic with a slash | top-right | while the microphone is off | mic live |
| **Input level** — meter | bottom band | while listening; height follows the envelope | not listening |
| **Working** — orbiting dot | above the face | while a turn is thinking, if it exceeds ~1.2 s | turn resolves |

Charge is otherwise expressed as **the face's own tiredness** (slower blinks, lower lids, reduced idle amplitude below ~15 %) rather than a number — the concept's rule, kept.

The **halo** (v5 / FIRE) is an indicator too: `accent_color` at the emotion's pattern. It never signals a fault; a fault is the error face plus the link glyph.

## Input — Core S3 (touch)

The face is the button. Gestures split into two families: **affection**, which the face answers with a reflex, and **control**, which the UI answers with chrome.

| Gesture | Zone | Family | Result |
|---|---|---|---|
| Tap | cheek, forehead | affection | tickle reflex; repeated taps build joy |
| Slow stroke | anywhere on the face | affection | contented arc eyes, blush |
| Poke | eye | affection | surprise + recoil |
| **Press-and-hold** | anywhere | control | **PTT** — the level meter appears while held; release sends the utterance |
| **Hold past 1.2 s in silence** | anywhere | control | the hold **becomes the skin carousel**: dots appear, slide left/right to choose, release to confirm |
| Swipe down from the top edge | — | control | the **status sheet** (below) |
| Swipe up / tap outside | — | control | dismiss the sheet |
| Tap the mic button | top-left corner | control | mute / unmute the microphone, with an audible click |

> **Mute is a button, not a gesture — and it took three attempts to get there.** This table said
> "two-finger tap" from the start and v2.4 implemented it, but on the board it never fired: the
> CoreS3's panel reports **one** touch point, `peak fingers=1` on every two-fingered tap, whatever
> the FT6336U's datasheet says the controller can do. The double tap was tried next and worked —
> but it spent a gesture this same table had already given to affection, so *"repeated taps build
> joy"* would have had to mean something else.
>
> That is the pattern worth recording: **every version of "mute is a gesture" collided with
> affection.** The two-finger tap leaked a tickle (v2.4 review #2), the single tap was mute's
> stop-gap home from v1.4 and blocked the tickle entirely, and the double tap would have taken the
> joy ladder. A control sharing a vocabulary with the character keeps finding its way back into it.
>
> So mute gets its own target instead: a microphone glyph in the **top-left corner** of the upper
> band, drawn in both states — cyan and filled when live, amber, hollow and slashed when muted. The
> hit area is 60×28, wider than the glyph, because a corner control is aimed at rather than looked
> at. It is the one place on this screen where a control is visible, and it earns that by being the
> control reached for in a hurry.
>
> It **never fades**, which extends the rule this document already had for a muted microphone: a
> control that fades out is a control nobody can find. And `pure/touch.h` keeps the button out of
> the tap run entirely — it is not affection's counter's business that someone pressed a button.
>
> Tapping it **clicks** — two short rising notes — because the person cannot otherwise tell the
> press landed without looking. The click never plays over a reply.
>
> `/mic on|off` over serial does the same thing and stays — it is what makes the device scriptable,
> and every manual test since v2.3 depends on it.

**Why the hold does two jobs.** The concept assigns both PTT and the carousel to a long press; they are layered by *duration and intent* rather than split into two gestures. A hold is PTT from the first millisecond — that is the common case and it must not wait. Only a hold that passes 1.2 s **with no speech detected** converts to the carousel, and the conversion is announced by the dots appearing. Speaking at any point keeps it a PTT hold.

### The status sheet

The one surface with words. A swipe down from the top edge slides a translucent panel over the lower two-thirds: skin name, link and IP, battery percentage, mic and camera state, volume, firmware version. It is **read-mostly** — the only controls are mute, volume and skin, because everything else belongs in server config. It auto-dismisses after 8 s of no touch. It is never shown during a turn.

## Input — FIRE (v6, three buttons)

No touch, so the gesture map collapses onto A/B/C, and the bottom band gains a **button bar**: three short labels aligned to the physical buttons, shown for 2 s after any press and whenever the carousel or sheet is open.

| Button | Short press | Hold |
|---|---|---|
| **A** | mute / unmute | PTT (→ carousel past 1.2 s, same rule) |
| **B** | previous skin | status sheet |
| **C** | next skin | volume |

Affection gestures have no equivalent and are simply absent; the IMU reactions carry that weight on this board.

## Notifications

Four kinds, distinguished by how long they persist and whether they use words at all.

| Kind | Looks like | Lifetime | Examples |
|---|---|---|---|
| **Whisper** | a glyph alone, no text, in the bottom band | 1.2 s | skin changed, muted, volume step |
| **Info** | glyph + one line | 2.5 s | "Wi-Fi connected", "Presence on" |
| **Warning** | glyph + one line, amber | 5 s, then collapses to its indicator | "Battery 15 %", "Weak signal" |
| **Fault** | glyph + one line + code, red | **until resolved** | "No server · `server_unreachable`" |

Rules: at most one notification at a time — a newer one of equal or higher rank replaces the current; a lower one waits, and is dropped if it is stale by the time the band frees. **Never notify what the character can say**: a greeting, a result, an answer to a question is speech, not a toast. Never notify a state the face already shows.

Faults show the enumerated `error.code` verbatim, because that string is the one thing worth typing into a search or a log. The face is simultaneously in `error`, so the screen reads as "something is wrong" even from across the room, and the code is there when you walk up to it.

## Screens

Beyond the running face, six full-screen states exist. Each is still a face plus chrome — no splash art, no logos.

| Screen | Face | Chrome |
|---|---|---|
| **Boot** | eyes closed, opening | none |
| **Wi-Fi connecting** | `calm`, gaze drifting | link arcs cycling |
| **Not configured** | `calm` | info line: the AP name to join |
| **Offline** | `sad`, dimmed to ~60 % | link crossed + fault line |
| **Fault** | `error` | fault line with the code |
| **Updating** | eyes closed | a thin progress arc along the bottom band |

## Motion and timing

| Thing | Timing |
|---|---|
| Reflex to a touch | < 100 ms, always local |
| Expression crossfade | 150–250 ms |
| Chrome fade in / out | 120 ms / 400 ms |
| Indicator settle-and-hide | ~3 s after the state stops changing |
| Hold → PTT | immediate (from ~120 ms of contact) |
| Hold → carousel | 1.2 s, only without speech |
| Toast lifetimes | whisper 1.2 s · info 2.5 s · warning 5 s · fault until resolved |
| Status sheet auto-dismiss | 8 s |

## What this adds to the contracts

Nothing structural — this is a rendering and input concept over the existing seams:

- `EmotionFrame` is unchanged. Chrome is not driven by it; it is driven by device-local facts (link, charge, camera power, mic state) and by `error{code}`.
- `event{}` gains no new `type`; the control gestures (carousel, sheet, mute) are **local UI**, not character events, and are deliberately *not* reported — the character has no opinion about your volume.
- `config_updated{face_set}` and the carousel are the same action from two directions, and the carousel's confirmation is reported in the next `hello`.
- The `caps` flags decide which input map is active: `touch` → the gesture table, `buttons` → the FIRE table.
