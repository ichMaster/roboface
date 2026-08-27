# Testing RoboFace

Everything here has been run. Commands are given **from the repository root** unless a block says
otherwise — the one exception is PlatformIO, which must run from `firmware/`.

```bash
cd ~/development/roboface
```

RoboFace is three tiers, and they are testable independently on purpose:

```
Core S3 firmware  ──WiFi/ws──▶  Python server  ──HTTPS──▶  Gemini
   §4 board            §5 both       §3 server
```

Most of what can be wrong is covered by §2, which needs no hardware, no API key and no network.

---

## 1. Setup, once

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt
```

For the board you also need PlatformIO (`brew install platformio`) and a `config.h`:

```bash
cp firmware/src/config.example.h firmware/src/config.h
$EDITOR firmware/src/config.h        # WiFi SSID/password, SERVER_URL, brightness
```

`firmware/src/config.h` and `server/.env` are **gitignored** — they hold the WiFi password and the
provider keys, and neither ever reaches GitHub.

---

## 2. The automated suites — no hardware, no key, no network

```bash
.venv/bin/pytest                        # 372 passed, 3 skipped
.venv/bin/mypy server tools             # strict
.venv/bin/ruff check server tests tools
```

```bash
cd firmware && pio test -e native       # 99 passed
```

Both run in CI on every push, and neither makes a paid API call. That is a rule the suite enforces
rather than merely obeys — `test_no_paid_provider_is_reachable_from_the_suite` asserts it.

### What lives where

| | Count | What it covers |
|---|---|---|
| `tests/contract` | 43 | the pinned seams — WS protocol, `LLMProvider`, and the **firmware mirror** |
| `tests/unit` | 274 | codec, router, orchestrator, config, logging, prompt, and the tools |
| `tests/integration` | 51 | full turns through the real ASGI app, driven by a fake device |
| `tests/live` | 2 | **the only paid tests** — excluded from collection, see §6 |
| `firmware/test/*` | 99 | the firmware's pure half: protocol mirror, state machine, backoff, line reader, URL parser, face recipes, chrome rules, layout |

`pytest` from the root collects `tests/` only — `codegen/` has its own suite, run from inside
`codegen/`, and `tests/test_repo_scaffold.py` asserts the two never merge.

### Two tests worth knowing about

**`tests/contract/test_firmware_mirror.py`** reads the C++ header `firmware/src/pure/ws_protocol.h`
and compares its constants, both message vocabularies and all twelve error codes to the server's
`protocol.py` — and feeds the exact bytes the firmware's `buildHello()` emits to the server's real
`decode()`. Two copies of a contract in two languages drift silently; this runs in the **Python**
job, so a drift is caught by someone who never invokes `pio`.

**`test_deltas_arrive_while_the_model_is_still_generating`** asserts *interleaving*, not a frame
count. Counting frames proves a reply was split, not streamed — a server that generated the whole
reply then sent it in five pieces would pass a counting test and fail every latency promise built
on it.

---

## 3. Testing the server on its own

The server runs on the Linux box, not on this machine — [DEPLOYMENT.md](DEPLOYMENT.md) covers why
and how. Start it and confirm it is answering:

```bash
tools/remote.sh start
tools/remote.sh health          # OK  …  port open, service answering
```

Then — `tools/chat.py` stands in for the device. It speaks the protocol through
`protocol.py`, exactly as the firmware does, so a contract change breaks it loudly rather than
letting it drift:

```bash
.venv/bin/python tools/chat.py
```

```
connected to ws://192.168.1.197:8000/ws  as terminal

you › Привіт! Що ти вмієш?
   Привіт! Я можу тобі компанію скласти, почути, що ти кажеш, і реагувати своїм обличчям.
   (3 deltas · first 596 ms · complete 912 ms)
```

**`first 596 ms · complete 912 ms` is the thing to look at.** Time-to-first-delta is the number the
whole architecture exists to keep small, and watching the reply arrive rather than appear is the
property v0.2 delivers.

### Running it on this machine instead

For server work that does not involve the board, loopback here is fine — it is only *inbound LAN*
connections this Mac refuses, and 127.0.0.1 is exempt from that:

```bash
PYTHONPATH=server .venv/bin/python -m roboface_server.app      # terminal 1
.venv/bin/python tools/chat.py --url ws://127.0.0.1:8000/ws    # terminal 2
```

The `.env` is **not** sourced into the shell. `load_settings()` reads `server/.env` itself, by a
path derived from the module's own location, so the server finds it from any working directory —
and sourcing it is actively wrong: `WEATHER_URL`'s value contains unquoted `&`, so the shell
backgrounds at the first one and the assignment is lost in a subshell. Silently, with exit code 0.

The board cannot be tested this way; see §5.

### Exercising the error paths

| Command | Expect |
|---|---|
| `/ping` | `pong` |
| `/bad` | `bad_frame` — malformed JSON |
| `/raw {"type":"sing_a_song"}` | `bad_frame` — unknown type |
| `/raw {"type":"emotion"}` | `bad_frame` — declared, not implemented in this version |
| `/binary` | `bad_frame` — binary frames carry no envelope |
| `/stats` | turns, deltas, and first-delta timings for the session |

Started with `--proto-ver 99`, the server answers `proto_unsupported`, closes the socket, and the
tool exits non-zero — so it is usable as a scripted smoke check.

---

## 4. Testing the board on its own

Flash it (**from `firmware/`**):

```bash
cd firmware && pio run -e cores3 -t upload
```

Watch it (from the root). Use the repo's monitor, not `pio device monitor` — see §6:

```bash
.venv/bin/python tools/board.py                  # watch, and type commands at it
.venv/bin/python tools/board.py --send /faces    # drive the self-test from a script
.venv/bin/python tools/board.py --for 30 --quiet
```

### The serial commands

| Command | What it proves |
|---|---|
| `/faces` | cycles the six states — **this is v0.4's DoD check** |
| `/chat-on` | the conversation on the panel — **this is v0.5's DoD check** |
| `/chat-off` | back to the face, and to the state it borrowed the screen from |
| `/debug` | the corner state line, on/off |
| `/help` | the rest |
| any other text | becomes a `text_in` — needs a reachable server (§5) |

```
[board] >>> /faces
[faces] cycling the six DoD states; the screen shows only the face.
[faces] 1/6  idle
[faces] 2/6  listening
...
[faces] done — back to wifi_connecting.
```

**Watch the screen, read the serial.** The screen deliberately never says which state it is in — a
device state is a face, never a word — so the serial log is how you check that what you are looking
at is what it should be. Six faces should be plainly different from each other, and each should
appear whole rather than flickering into place.


### The chat console — v0.5's DoD check

The face shows a turn's *state*; the console shows its *content*. It borrows the screen the way
`/faces` does, and gives it back:

```bash
.venv/bin/python tools/board.py
```
```
/chat-on
[chat] console on — type a message; /chat-off returns the face.
chat> Склади одне речення зі словами: їжак, ґанок, єдність, місто.

[state] thinking
[state] replying
  Їжак на ґанку спостерігав за містом, мріючи про єдність усіх його мешканців.
[state] idle
chat>
```

**Read that sentence off the panel, not off the serial log.** The serial half proves the turn
worked; the DoD is about the screen. Four things to look at:

- **The Ukrainian is legible** — no boxes, no half-characters at a line break. The question above is
  built to force `ї`, `ґ`, `і` and `є`, the letters Russian does not use and that a font advertised
  as "Cyrillic" can quietly omit.
- **The question is dimmer than the answer.** That is the only thing distinguishing them: the screen
  never labels what it is showing, so weight does the work a word would otherwise do.
- **A long reply clips rather than overruns.** Ask for something lengthy — *"Розкажи детально про
  історію Києва, щонайменше 8 речень"* — and the panel should show the tail of it, still inside the
  face area, never touching the chrome bands.
- **`/chat-off` gives back exactly what it took.** Not "idle" — whatever was showing. Entering the
  mode while the board is still associating and leaving it should return you to `wifi_connecting`.

**Known font gap.** The embedded face covers ASCII and `U+0400`–`U+0523`. General Punctuation is
outside that, so an en dash (`–`), an em dash (`—`) or a typographic apostrophe (`’`) — all of which
a model writing Ukrainian prose reaches for — draw as nothing rather than as a wrong letter. Words
are unaffected.

### What a healthy boot looks like

```
RoboFace firmware 0.4.0 on m5stack-cores3
[state] wifi_connecting
[net] connecting to "..." (attempt 1)
[net] up, ip 192.168.1.131
[ws] server 192.168.1.197:8000/ws
[status] wifi_connecting · link up 192.168.1.131 · ws disconnected · batt 100% · up 20s
```

If the socket cannot connect you will see the backoff growing with jitter — `470, 832, 1680, 3136,
7460 ms` — capped near 30 s. That is correct behaviour, not a bug: it means the board is fine and
the server is unreachable. Go to §5.

---


### Speech — v1.1's DoD check

From v1.1 a turn is answered **out loud**. The check has two halves, and the second is the one that
can silently regress:

```bash
tools/remote.sh start
.venv/bin/python tools/board.py
```
Then type a line and listen:
```
Привіт! Розкажи коротко, хто ти такий.
[state] thinking
[state] replying
  Привіт! Я — RoboFace, твій настільний компаньйон з живим обличчям…
```

**1. It speaks.** Sound comes out of the board's speaker while the text is still arriving. The first
spoken phrase should be a whole word — if it begins mid-word the phrase splitter cut early.

**2. It speaks *before* it has finished writing.** This is the property v1.1 exists for, and by ear
it is indistinguishable from a fast request/response pipeline. Read it off the log instead:

```bash
tools/remote.sh logs -n 40 | grep -E 'turn\.(speaking|reply)'
```
```
"ts": "…T17:16:26.620Z", "event": "turn.speaking", "deltas_so_far": 2
"ts": "…T17:16:29.491Z", "event": "turn.reply",    "deltas": 3
```

`turn.speaking` is written when the first `tts_audio` frame leaves; `turn.reply` when the model's
last delta arrives. **`turn.speaking` must come first.** Above it does, by 2.9 s, with only two of
three deltas written. If the order ever reverses, something in the pipeline has started
accumulating and the phase has regressed however good the audio sounds.

**This check costs money.** The suite is fully mocked and always will be, but hearing the board
speak means a real ElevenLabs call. It is the only paid step in this document, and it is a manual
DoD check rather than a test.

**If it is silent but the text works,** the server is running without TTS configured — which is a
deliberate state, not a fault. Look for it:

```bash
tools/remote.sh logs -n 100 | grep tts.disabled
```


### Hearing — v1.2's DoD check

**Hold the face, speak, release.** The claim is that speech reaches the server *while you are still
talking*, and by feel that is indistinguishable from a device that uploads at the end — so it is
read off the log, not judged.

```bash
tools/remote.sh logs -n 400 | grep -E 'listen\.(start|stop)'
```
```
listen.start                              +0.00 s
listen.stop   bytes=96640 frames=151      +3.07 s
```

For the per-frame evidence, put the server in debug for one run — `ROBOFACE_LOG_LEVEL=debug` in
`server/.env`, then `tools/remote.sh restart`:

```
first audio   +0.07 s     ← 151 frames spread evenly across the window
last audio    +3.06 s
listen.stop   +3.07 s
```

**The first frame must arrive seconds before `listen_stop`, and the frames must be spread rather
than bunched.** A burst just before the stop means the device buffered the utterance, which v1.3's
recognition cannot work with.

`/listen [s]` does the same from a script, without a finger on the glass — useful because it makes a
capture failure reproducible.

**`/loopback [s]`** records and replays locally, sending nothing to the server. It reports the peak
level it captured, which is the fastest way to tell a dead microphone from a dead speaker:

```
[loopback] captured 48 frames (960 ms), peak 53% — playing back
```

**A peak near 1% means the gain is wrong, not the microphone.** M5Unified's default magnification of
16 leaves this board's ES7210 at about that; `MIC_GAIN` in `firmware/src/config.h` is 64, which puts
normal speech at 30–70%. That figure is worth checking whenever recognition quality drops — it is a
one-line cause with a symptom that looks like a model problem.

Loopback is bounded by the playback buffer (~1 s, since PSRAM reports zero free on this board) and
says so rather than silently recording less than asked.


### The voice loop — v1.3's DoD check

Hold the face, speak Ukrainian, release. You should be answered aloud.

The claim is not that it works but that it is **quick for a specific reason**, so the check is three
numbers rather than a stopwatch:

```bash
tools/remote.sh logs -n 400 | grep -E 'asr.resolved|turn.first_delta_ms|turn.speaking'
```
```
asr.resolved         ms=180
turn.first_delta_ms  ms=~700
turn.speaking        ms_since_listen_stop=~1200
```

**The ASR leg must be the smallest of the three.** That is the whole design: recognition runs while
you speak, so when you stop the transcript already exists and only the endpointing window remains.
If that number is the *largest*, recognition is running in batch somewhere and the phase has missed
its point however well it works.

First audio should follow `listen_stop` by under ~1.5 s on the development network.

**A breathing pause must not trigger an answer.** Say a few words, pause for about a second
mid-sentence, then finish. One answer should come, to the whole sentence. Two answers means the
phrase-hold is not holding.

The serial channel shows what was heard, and only there — the screen still shows states:

```
[heard?] прив          ← a guess, revised in place
[heard] Привіт, як справи?
```

**Without a `DEEPGRAM_API_KEY` the device still types.** Speech input is optional exactly as speech
output is; look for `asr.disabled` in the log.

## 5. Testing both together

The board dials the server over the LAN, so the server has to live somewhere the board can reach.
It runs on the Linux box `ich-picobox` (`192.168.1.197`); [DEPLOYMENT.md](DEPLOYMENT.md) is the
deployment and administration guide.

```bash
tools/remote.sh start && tools/remote.sh health
.venv/bin/python tools/board.py --for 40
```

A working link, on the board's serial output:

```
[ws] server 192.168.1.197:8000/ws
[ws] connected
[state] idle
```

Confirm it from the server side — the board shows up as a live session, by address:

```bash
tools/remote.sh status
```
```
  sessions  1 established
            <- 192.168.1.131:52565
```

`SERVER_URL` is compiled into the firmware and **must be a literal IP**. The firmware links no mDNS
resolver, so `ich-picobox.local` — a name the host-side tools could otherwise use — never resolves
there and the board simply never connects.

### Why the server does not run on this Mac

It cannot host the board's connection. The workstation's endpoint filtering accepts an inbound LAN
connection and then destroys the socket before the application's first read:

```
ACCEPTED from 127.0.0.1:58600     -> read 77 bytes -> replied 200
ACCEPTED from 192.168.1.64:58603  -> read/write FAILED: errno 57 (Socket is not connected)
```

It is a **socket**-layer filter, not a packet filter, which is what makes it so easy to misdiagnose:
the TCP handshake completes in the kernel, so `nc -z` reports the port "open" and the board's own
probe reported "connected" — only the first `read()` fails. What was established:

- **not** the Application Firewall's per-app allowlist — an explicitly allowed binary (`node`) fails
  identically on the same port
- **not** the router, and nothing there needs changing — `route -n get 192.168.1.64` resolves to
  `lo0`, so the Mac's probe of its own address never leaves the machine, and the board reaches a
  *different* host on that same LAN perfectly
- two managed-Mac network extensions are active — `GlobalProtectExtension` and
  `com.sentinelone.network-monitoring`, both socket filters. Which of them does it was not
  determined; interrogating either needs root.

A public tunnel is not a way round it either: the firmware speaks `ws://` only, with no TLS.

Everything on either side of that link is verified independently, by §3 and §4.

---

## 6. Gotchas, each of which cost real time

**A flash cannot connect** (`Device not configured`, or the port vanishing from `/dev` mid-upload).
The Core S3's USB-C is the ESP32-S3's *native* USB. A board running M5Stack's UiFlow presents a CDC
device that does not survive esptool's reset handshake. Hold the power button ~2 s until the green
LED lights to force the ROM bootloader, then flash. **Once this firmware is on the board the problem
goes** — it uses the hardware USB-Serial/JTAG controller (`303A:1001`), which handles the reset in
silicon, and no button press is needed again.

**The monitor drops on every reset.** Same cause: native USB re-enumerates, so the port disappears
and returns as a new device. That is the board restarting, not a broken cable — and it is why
`pio device monitor` dies at exactly the moment you want to be watching. `tools/board.py`
reattaches.

**The board joins WiFi but never connects** — `[ws] attempt N failed` climbing to the ceiling while
the server logs nothing. That is §5, not a firmware fault.

**Nothing on the serial port at all, but the screen works.** `Serial` on the ESP32-S3 defaults to
the hardware UART pins. `platformio.ini` sets `-DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1`
for exactly this; without them the debug channel writes to GPIO43/44 and the board looks dead over
USB while running perfectly.

**Association refused temporarily, comeback time … too long.** The access point is deferring
association with a window longer than the ESP32 accepts. Intermittent; the board retries and
usually gets in. Not a firmware fault.

---

## 7. The one paid test

```bash
ROBOFACE_LIVE_TESTS=1 .venv/bin/pytest tests/live       # 2 passed, makes real Gemini calls
```

Guarded **twice**, because a single guard that silently stops guarding leaves an identical-looking
green run: `tests/live` is excluded from default collection *and* every test in it skips unless the
variable is exactly `1`. The gate is that variable rather than the presence of an API key — a key is
present on a developer's machine all the time, and gating on it would make "run the suite" a paid
operation. `test_the_live_suite_cannot_run_by_accident` asserts both guards.

---

## 8. What each layer independently proves

| Layer | Verified by | Property |
|---|---|---|
| Wire contract | contract tests, both languages | the two implementations cannot drift apart unnoticed |
| Streaming | `test_deltas_arrive_while_the_model_is_still_generating` | deltas leave as they arrive — checked against a deliberately buffering implementation |
| Turn semantics | `tests/unit/test_orchestrator.py` | six endings enumerated; history always completes or rolls back |
| Error attribution | `tests/unit/test_error_mapping.py` | `bad_frame` is the device's fault, `internal` is the server's, and the LLM leg claims neither |
| Firmware logic | `pio test -e native` | state machine total over (state, event); backoff capped and jittered |
| Board | `tools/board.py`, on hardware | WiFi, URL parsing, state machine, backoff — all observed |
| Face | `/faces`, by eye | six distinct faces, no tearing |

---

## 9. Reference

| | |
|---|---|
| Server host | `ich-picobox` — `192.168.1.197`, driven by [tools/remote.sh](tools/remote.sh) |
| Product server port | `8000` (`ROBOFACE_WS_PORT`) — `codegen/`'s dashboard owns `8420` |
| Board address | `192.168.1.131` (DHCP), device id `core-s3-01` |
| Board | `m5stack-cores3`, ESP32-S3, 16 MB flash, 8 MB PSRAM |
| Transport | `ws://` — the server runs uvicorn with **no TLS**; see ARCHITECTURE §Contracts |
| Protocol version | `1`, with no compatibility window |
| Secrets | `server/.env`, `firmware/src/config.h` — both gitignored |

```bash
tools/remote.sh start | stop | restart | status | health | logs [-f] | ping | shell | help
```

- [DEPLOYMENT.md](DEPLOYMENT.md) — deploying and administering the server, and the firmware
- [CLAUDE.md](CLAUDE.md) — the toolchain commands and the repository map
- [specification/ARCHITECTURE.md](specification/ARCHITECTURE.md) — contracts, seams, testing policy
- [specification/features/DEVICE_UI.md](specification/features/DEVICE_UI.md) — what the screen shows
- [codegen/README.md](codegen/README.md) — the generation tracker, which has its own suite
