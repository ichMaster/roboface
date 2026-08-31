# Skin assets — what to draw, and what the panel can show

**For:** whoever draws the five faces. **Against:** a 2.0″ 320×240 IPS panel on an M5Stack Core S3.

The five faces are currently drawn from circles and rectangles by the firmware, which was enough to
prove the architecture and is not enough to look at. This document is what has to exist instead, in
enough detail that it can be drawn without asking a second question.

---

## 1. The architecture you are drawing into

**The face is not one picture. It is a body plus a changing expression**, and the expression is
shared by all five skins. That is the design, not a shortcut: `specification/face-prototype.html`
draws every skin by calling the same two functions with a different colour and position, and this
project's rule is that a skin which needs its own logic means the design is wrong.

So the work splits in two, and the second half is drawn **once**:

| | how many | drawn per skin? |
|---|---|---|
| **A. Bodies** — the silhouette and its background | 5 | yes |
| **B. Features** — eyes and mouths, every state | 19 | **no — one set, shared** |
| **C. Element overlays** — the one thing each spirit does | 5 | yes, where it applies |

**29 images in total.** The features are shared because they are the character; the bodies are the
costume. A ghost and a flame are the same creature in different clothes, and that is why switching
skins does not switch personalities.

> **If 29 is too many to start with, draw the five bodies first.** They are most of what looks
> wrong, they need no firmware change, and the procedural features already work. The feature set can
> follow.

---

## 2. The canvas

```
        0                                                        320
      0 ┌────────────────────────────────────────────────────────┐
        │  ▓▓ chrome band — 28 px — NOTHING MAY BE DRAWN HERE ▓▓  │
     28 ├───┬────────────────────────────────────────────────┬───┤
        │ ▓ │                                                │ ▓ │
        │ ▓ │                                                │ ▓ │
        │ ▓ │            the face safe area                  │ ▓ │
        │ ▓ │              264 × 184                         │ ▓ │
        │ ▓ │            x 28…292 · y 28…212                 │ ▓ │
        │ ▓ │                                                │ ▓ │
    212 ├───┴────────────────────────────────────────────────┴───┤
        │  ▓▓ chrome band — 28 px — NOTHING MAY BE DRAWN HERE ▓▓  │
    240 └────────────────────────────────────────────────────────┘
```

- **Deliver every body at the full 320 × 240.** The background is yours: the outer bands will be
  painted over by the firmware, but drawing to the edge means no seam at the boundary.
- **The 28 px bands carry chrome** — the link and battery top-right, the microphone button
  top-left, the face-picker button bottom-left, the level meter and the toast. Anything you draw
  there is covered. Keep the silhouette's meaningful parts inside `y 28…212`.
- **The two corner buttons are always visible** and must not be competed with: a microphone glyph at
  top-left and a two-faces glyph at bottom-left, each about 20 px in an 84 × 44 touch target.

---

## 3. Colour — what the panel can actually show

**RGB565.** Every colour becomes 5 bits of red, 6 of green, 5 of blue: **32 levels of red, 64 of
green, 32 of blue**, 65,536 in total. Practical consequences, in order of how often they bite:

1. **Large smooth gradients band**, and they band worst in blue and red, which have half green's
   resolution. A sky fading over 200 px crosses at most 32 distinct blues — you will see every step.
   Use flat fills, or gradients with visible texture, or dither.
2. **Very dark colours collapse.** Below about `#101010` the panel's own black swallows the
   difference. Do not rely on separation between `#0a0d1c` and `#0d1020`; on the desk they are one
   colour.
3. **Very light colours clip.** Above `#f0f0f0` the same thing happens at the other end.
4. **Saturated greens survive best**, saturated blues worst. A blue-on-blue design will look muddier
   on the device than on your monitor.

**Quantise before you judge it.** `#5DFF` in the firmware is `#58BFFF` on your screen — the
conversion drops the low bits:

```
r565 = round(r / 255 * 31)   g565 = round(g / 255 * 63)   b565 = round(b / 255 * 31)
```

**Colours already in use, which are the character's and should be worked with rather than around:**

| what | hex | where |
|---|---|---|
| the resting cyan-white ink | `#5DFFFF` | features, procedural face |
| night sky | `#0A0D1C` | behind the ghost, flame, jelly |
| indicator cyan | `#00FFFF` | link, level meter, active mic, picker arrows |
| amber | `#FFA400` | muted mic, discharging battery |
| fault red | `#F80000` | error code — **reserved, use nothing near it for decoration** |

**Alpha does not exist on the device.** The framebuffer is opaque 16-bit. Alpha in your files is used
once, at composite time, and then it is gone — so a feature drawn at 50 % opacity over the body will
be flattened against *that skin's* body colour and cannot be re-composited later.

---

## 4. A. The five bodies — one image each

**Format:** PNG, 320 × 240, no transparency needed (draw the background too).
**Naming:** `body-stackchan.png`, `body-ghost.png`, `body-flame.png`, `body-jelly.png`,
`body-cloud.png`.

**The hole the features go in.** Every body must leave these two rectangles readable — the shared
features are blitted there, in that skin's ink colour, and anything busy underneath makes the
expression unreadable:

```
        eyes:   x  76 … 244    y  64 … 152     (168 × 88, centred on 160,108)
        mouth:  x 100 … 220    y 132 … 204     (120 × 72, centred on 160,168)
```

Both shift **up to 14 px horizontally** when the face looks toward a voice, so leave that much
margin. Draw the body as if a face will be placed on it — because one will.

**Each body in a sentence, plus what makes it that creature:**

- **stackchan** — no body. A dark field with a faint wash behind where the head would be. This is
  the fallback face and the one that must always work: keep it simple and legible.
- **ghost** — a rounded dome with a scalloped hem, softly luminous, floating on a night sky with a
  few stars. Off-white `#F2F4FF` against `#0A0D1C`. The blush and tear (§6) sit on it.
- **flame** — a teardrop, wide at the base, tapering upward, with the hot core lighter than the
  edges. **Draw it in the resting orange**; the firmware recolours the whole body per emotion, so
  the shape must read at any hue. Avoid detail that only works when it is orange.
- **jelly** — a translucent bell with tendrils below, a few bright spots on the dome. Same rule:
  **the firmware recolours it**, so the form has to survive being pink, violet or blue.
- **cloud** — overlapping lobes with a flat base, on a daylight sky. Recoloured per emotion too:
  bright white when content, washed pale when sad, overcast grey on a fault.

> **Three of the five are recoloured at runtime.** Draw them in a **neutral mid-tone with the shading
> carried by lightness, not hue** — the firmware multiplies a single colour across the body, so a
> body drawn with warm highlights and cool shadows will fight it. Think of them as greyscale forms
> that will be tinted.

---

## 5. B. The features — one shared set, 19 images

**Format:** PNG with alpha, **drawn in pure white** on transparency. The firmware tints them to each
skin's ink colour, which is what makes one set serve five faces. Anti-aliased edges are welcome and
help a lot at this size.

**Two canvases only:**

- **eye pairs:** 168 × 88 — both eyes in one image, so you control the asymmetry
- **mouths:** 120 × 72

### Eyes — 8 images

| file | state | what it says |
|---|---|---|
| `eyes-neutral.png` | resting, ~85 % open | the face everything else is a departure from |
| `eyes-calm.png` | wide, slightly softened | attentive, listening |
| `eyes-joy.png` | happy arcs, closed upward | delight |
| `eyes-thinking.png` | narrowed to slits, ~45 % | working on it |
| `eyes-surprised.png` | fully round, highlight visible | startled |
| `eyes-sad.png` | drooping, ~40 % open, outer ends down | unhappy |
| `eyes-error.png` | crosses | the fault face |
| `eyes-closed.png` | shut — a line | **blinking**, used constantly by every emotion |

### Mouths, at rest — 7 images

`mouth-neutral · calm · joy · thinking · surprised · sad · error` — the same seven, as mouths. The
grammar the firmware already uses: joy is a filled upward curve, thinking is a small off-centre
line, surprised is a vertical oval, sad is a downward curve, error is a zigzag.

### Mouths, speaking — 4 images

These replace the emotion mouth **while the device is talking**, driven by the loudness of its own
voice about 50 times a second. They must read as one mouth moving, not as four different mouths:

| file | opening |
|---|---|
| `mouth-ajar.png` | barely parted |
| `mouth-half.png` | half open, narrower |
| `mouth-wide.png` | open and spread |
| `mouth-open.png` | fully open, rounded |

The closed viseme reuses `mouth-neutral.png`, so there is no fifth file.

> **What separates these at a metre is direction, not detail.** The firmware's own note on the
> expression table says it plainly: *"two emotions that differ only in mouth curvature are two
> emotions nobody can tell apart at 320×240 from a metre away."* Push the brows and the eye aperture;
> the mouth confirms what the eyes already said.

---

## 6. C. Element overlays — 5 images

The one thing each spirit does that the others do not. Same format as the features: PNG with alpha,
**but in their own colours** — these are not tinted.

| file | size | skin | when |
|---|---|---|---|
| `elem-ghost-blush.png` | 168 × 40 | ghost | pink cheeks, on joy / neutral / surprised |
| `elem-ghost-tear.png` | 24 × 40 | ghost | one tear, on sad only |
| `elem-jelly-glow.png` | 200 × 120 | jelly | the bright spots on the bell, always |
| `elem-cloud-sun.png` | 72 × 72 | cloud | a sun in the upper right, on joy only |
| `elem-cloud-rain.png` | 200 × 60 | cloud | rain below the lobes, on sad only |

The flame has no overlay: its element **is** the body's colour.

---

## 7. What to send back

The PNGs, and for each body **the eye and mouth anchor points you actually drew to** if they differ
from §4 — those are two numbers per body and they go straight into the manifest.

Nothing else. No sprite sheets, no atlases: the firmware assembles them.

## 8. Why this is a spec and not a mood board

Everything above is a number the firmware already uses, not a preference. The safe area is
`layout.h`; the seven emotions and their eye apertures are `face.h`'s recipe table; the four visemes
are `lipsync.h`; the tint-per-skin arrangement is `skin.h`'s manifest, which exists so that adding a
skin needs no renderer code.

Two of those numbers were established the hard way and are worth not re-litigating: the 28 px bands
are **never** drawn into because chrome that a face paints over is chrome that lies, and the features
shift 14 px because the face turns toward whoever is speaking.
