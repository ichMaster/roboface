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

Terminal 1:

```bash
set -a && . ./server/.env && set +a
PYTHONPATH=server .venv/bin/python -m roboface_server.app
```

Terminal 2 — `tools/chat.py` stands in for the device. It speaks the protocol through
`protocol.py`, exactly as the firmware does, so a contract change breaks it loudly rather than
letting it drift:

```bash
.venv/bin/python tools/chat.py
```

```
connected to ws://127.0.0.1:8000/ws  as terminal

you › Привіт! Що ти вмієш?
   Привіт! Я можу тобі компанію скласти, почути, що ти кажеш, і реагувати своїм обличчям.
   (3 deltas · first 596 ms · complete 912 ms)
```

**`first 596 ms · complete 912 ms` is the thing to look at.** Time-to-first-delta is the number the
whole architecture exists to keep small, and watching the reply arrive rather than appear is the
property v0.2 delivers.

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
.venv/bin/python tools/board.py                  # until Ctrl-C
.venv/bin/python tools/board.py --send /faces    # and drive the self-test
.venv/bin/python tools/board.py --for 30 --quiet
```

### The serial commands

| Command | What it proves |
|---|---|
| `/faces` | cycles the six states — **this is v0.4's DoD check** |
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

### What a healthy boot looks like

```
RoboFace firmware 0.4.0 on m5stack-cores3
[state] wifi_connecting
[net] connecting to "..." (attempt 1)
[net] up, ip 192.168.1.131
[ws] server 192.168.1.64:8000/ws
[status] wifi_connecting · link up 192.168.1.131 · ws disconnected · batt 100% · up 20s
```

If the socket cannot connect you will see the backoff growing with jitter — `470, 832, 1680, 3136,
7460 ms` — capped near 30 s. That is correct behaviour, not a bug: it means the board is fine and
the server is unreachable. Go to §5.

---

## 5. Testing both together

The board needs to reach the server over the LAN. Point `SERVER_URL` in `firmware/src/config.h` at
the machine running it — **not `localhost`**, which on the board means the board:

```c
#define SERVER_URL "ws://192.168.1.64:8000/ws"
```

Find that address with `ipconfig getifaddr en0`. Then flash, start the server, and type a line into
`tools/board.py`. A working turn looks like the reply streaming back over serial.

### Known blocker on a managed Mac

On this development machine it does not work, and the cause is not in the code: **the managed
security policy refuses inbound LAN connections to the server process.** The board retries
correctly and the server logs *zero* connection attempts. Diagnosed and confirmed:

- loopback works (`curl http://127.0.0.1:8000/ws` → 404, the correct answer for a WebSocket route)
- the LAN address is refused in ~2 ms — an active reset, not a timeout or a dropped packet
- the Mac can ping the board, and ARP resolves, so the network path is fine
- `socketfilterfw` cannot be changed from the command line: *"Firewall settings cannot be modified
  from command line on managed Mac computers"*
- putting an allow-listed binary in front of it (a Node TCP relay) is refused the same way

Three ways round it, in order of least friction:

1. **Run the server on another machine** on the same LAN — a Pi, a Linux box, a VM — and point
   `SERVER_URL` there. Nothing to argue with.
2. **An outbound tunnel**: `cloudflared tunnel --url http://localhost:8000`, then set `SERVER_URL`
   to the public host. The laptop connects *outward*, so no inbound rule is needed.
3. **Ask IT** to allow inbound connections to this Mac.

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
| Product server port | `8000` (`ROBOFACE_WS_PORT`) — `codegen/`'s dashboard owns `8420` |
| Board | `m5stack-cores3`, ESP32-S3, 16 MB flash, 8 MB PSRAM |
| Transport | `ws://` — the server runs uvicorn with **no TLS**; see ARCHITECTURE §Contracts |
| Protocol version | `1`, with no compatibility window |
| Secrets | `server/.env`, `firmware/src/config.h` — both gitignored |

- [CLAUDE.md](CLAUDE.md) — the toolchain commands and the repository map
- [specification/ARCHITECTURE.md](specification/ARCHITECTURE.md) — contracts, seams, testing policy
- [specification/features/DEVICE_UI.md](specification/features/DEVICE_UI.md) — what the screen shows
- [codegen/README.md](codegen/README.md) — the generation tracker, which has its own suite
