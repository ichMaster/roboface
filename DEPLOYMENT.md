# Deployment and administration

How the RoboFace server gets onto the machine that runs it, how to drive it once it is there, and
how the firmware is built, flashed, and pointed at it. [TESTING.md](TESTING.md) is the companion:
it covers the suites and how to exercise what is running. This document covers getting it running.

Everything below has been run.

---

## Topology

Three machines, and which one does what matters:

| Role | Host | Address | What runs there |
|---|---|---|---|
| **Server** | `ich-picobox` (Ubuntu 22.04) | `192.168.1.197` | `roboface_server` on `:8000` |
| **Device** | M5Stack Core S3 | `192.168.1.131` | the firmware, joins WiFi, dials the server |
| **Workstation** | the Mac | `192.168.1.64` | the repo, `pio` (USB-attached board), `tools/*` |

The board and the server must be on the same LAN; the workstation only needs SSH to the server and
USB to the board.

### Why the server does not run on the workstation

The Mac cannot host it. Its endpoint filtering accepts an inbound LAN connection and then destroys
the socket before the application's first read:

```
ACCEPTED from 127.0.0.1:58600     -> read 77 bytes -> replied 200
ACCEPTED from 192.168.1.64:58603  -> read/write FAILED: errno 57 (Socket is not connected)
```

This is a socket-layer filter, not a packet filter, which is why the failure is so confusing:
the TCP handshake completes (so `nc -z` reports the port "open"), and only the first `read()`
fails. It is not the macOS Application Firewall's per-app allowlist — an explicitly allowed
binary fails identically — and it is not the router: `route -n get 192.168.1.64` resolves to
`lo0`, so those packets never leave the machine. Two managed-Mac network extensions are active
(`GlobalProtectExtension`, `com.sentinelone.network-monitoring`); which of them does it was not
established, and needs root to interrogate.

The board hit the same wall from the other side, and the fix for both was to move the server to a
Linux box. **Nothing needs changing on the router.**

---

# Part 1 — The server

Everything is driven by [tools/remote.sh](tools/remote.sh) over plain SSH. There is no systemd unit
and no agent on the box: start/stop is a PID file and a log file, and no command needs `sudo`.

## One-time setup

The script is non-interactive by design, so key-based SSH must work first:

```bash
ssh-copy-id ich@192.168.1.197     # once; asks for the password
ssh ich@192.168.1.197 true        # must succeed without prompting
```

Then deploy. The first run also installs the Python toolchain:

```bash
tools/remote.sh deploy
```

## Configuration

Defaults are baked in and overridden from the environment — no config file to keep in sync:

| Variable | Default | Meaning |
|---|---|---|
| `RF_REMOTE` | `ich@192.168.1.197` | `user@host` of the server box |
| `RF_REMOTE_DIR` | `roboface` | deployment directory, relative to `$HOME` on the box |
| `RF_REMOTE_PORT` | `8000` | the port the server binds |

```bash
RF_REMOTE=ich@192.168.1.50 tools/remote.sh status     # a different box, one command
```

The port must also match `ROBOFACE_WS_PORT` in `server/.env`, which is what the server actually
binds; `RF_REMOTE_PORT` is what the script checks and reports.

## Day-to-day

```bash
tools/remote.sh deploy [--dev]      # sync the repo, build the venv, install dependencies
tools/remote.sh start               # idempotent: a running server is left alone
tools/remote.sh stop
tools/remote.sh restart
tools/remote.sh status              # PID, listener, live sessions, reachability
tools/remote.sh health [--watch N]  # healthcheck, exit 0/1
tools/remote.sh logs [-f] [-n N]    # tail the server log
tools/remote.sh ping                # a real WS handshake -- no LLM call, so free
tools/remote.sh shell               # interactive ssh, in the deployment directory
```

### `health` — the monitor

Two checks, asked from the **workstation**, not from the box. A listener the board cannot reach is
not a running service, which is the whole reason the server moved off the Mac:

1. **Is the port open?** TCP reaches something.
2. **Does the service answer?** A real `hello` + `ping` through `tools/chat.py`, which speaks the
   protocol via `protocol.py` — so a wire-contract change fails this loudly instead of letting a
   healthcheck pass against a server that no longer speaks it.

```
OK       15:04:33  192.168.1.197:8000  port open, service answering      exit 0
DEGRADED 15:03:34  192.168.1.197:8000  port open, but the service did not answer   exit 1
DOWN     15:02:10  192.168.1.197:8000  port not reachable                exit 1
```

It exits `0` only when both pass, so it composes — `tools/remote.sh health && ...`. `--watch N`
polls every `N` seconds instead of exiting.

The three states are worth distinguishing: **DOWN** is the box or the network, **DEGRADED** is the
application — it is listening but not answering the protocol, which is what a bad `GEMINI_API_KEY`,
a wedged event loop, or a contract mismatch looks like from outside.

### `status` vs `health`

`status` describes; `health` judges. `status` is what you read when something is wrong — it shows
the PID and uptime, the listener, and every established session with its peer address, so you can
see at a glance whether the board is attached:

```
==> remote
  running   pid 6329, up 01:46
  listener  LISTEN 0  2048  0.0.0.0:8000  0.0.0.0:*
  sessions  1 established
            <- 192.168.1.131:52563          <- the board
  python    Python 3.13.15
  log       12K
==> from this machine
  tcp       192.168.1.197:8000 reachable
```

## Starting it by hand — `tools/remote.sh shell`

`start` runs the server detached, which is what you want almost always. When you want to *watch* it
start — a suspect `.env`, a provider that will not authenticate, a crash a second after boot —
run it in the foreground instead.

`shell` drops you straight into the deployment directory:

```bash
tools/remote.sh shell
```
```
ich@ich-picobox:~/roboface$
```

Stop the managed instance first, or the port is already taken:

```bash
tools/remote.sh stop
```

Then, on the box, the same command `start` runs — the module, with `server/` on the path:

```bash
PYTHONPATH=server .venv/bin/python -m roboface_server.app
```
```
INFO:     Started server process [5614]
INFO:     Waiting for application startup.
INFO:     Application startup complete.
INFO:     Uvicorn running on http://0.0.0.0:8000 (Press CTRL+C to quit)
```

Every log line — including each `hello.negotiated` as the board attaches — now streams to your
terminal instead of `var/server.log`. **Ctrl-C** stops it.

The `.env` is **not** sourced into the shell. `load_settings()` reads `server/.env` itself, by a
path derived from the module's own location, so the server finds it from any working directory —
and sourcing it is actively wrong: `WEATHER_URL`'s value contains unquoted `&`, so the shell
backgrounds at the first one and the assignment is lost in a subshell. Silently, with exit code 0.

`_resolve()` checks the process environment *before* the file, so a shell-mangled value would not
merely be lost — it would take precedence over the correct one in the file. Export a variable
deliberately to override a single setting for one run; never bulk-source the file.

### What is different about a hand-started server

Three things, and each has bitten:

- **It dies when you leave.** No `nohup`, so closing the SSH session or dropping the WiFi kills it.
  `start` survives both.
- **It writes no PID file.** `tools/remote.sh status` will report `stopped (no live pid file)` while
  a listener is plainly there and the board is connected — the status is describing the *managed*
  instance, not that one.
- **`stop` will not kill it**, because `stop` only knows the PID it wrote. It does notice, and says
  so rather than reporting a misleading success:

  ```
  note: another roboface_server process is still running:
    5614 .venv/bin/python -m roboface_server.app
    stop it with: pkill -f roboface_server.app
  ```

To hand control back, Ctrl-C it and run `tools/remote.sh start` from the workstation.

### Other things worth a shell

```bash
tools/remote.sh shell
: > var/server.log                                  # truncate the log; nothing rotates it
grep -c . var/server.log                            # how much has accumulated
VIRTUAL_ENV="$PWD/.venv" ~/.local/bin/uv pip list   # what is actually installed
.venv/bin/python -c 'import sys; print(sys.version)'
```

## Without the script — plain SSH

Nothing above is required. `tools/remote.sh` only wraps ordinary SSH, and every operation has a
direct equivalent for a machine that has no checkout of this repository.

### Start it, in the foreground

```bash
ssh ich@192.168.1.197
cd ~/roboface
PYTHONPATH=server .venv/bin/python -m roboface_server.app
```

### Start it, detached, in one command

```bash
ssh ich@192.168.1.197 'cd ~/roboface && \
  { PYTHONPATH=server nohup .venv/bin/python -m roboface_server.app \
      >> var/server.log 2>&1 </dev/null & echo $! > var/server.pid; }'
```

The redirections are not decoration. `ssh` returns when the remote's streams close, not when the
remote command exits — a background job still holding stdout keeps the session open and the command
appears to hang. Redirecting all three (`>>`, `2>&1`, `</dev/null`) is what lets `ssh` return
immediately, which it does: measured at 0 s.

Writing `var/server.pid` keeps this compatible with `tools/remote.sh stop` and `status` later.

### Stop it

```bash
ssh ich@192.168.1.197 'kill "$(cat ~/roboface/var/server.pid)" && rm -f ~/roboface/var/server.pid'
ssh ich@192.168.1.197 'pkill -f roboface_server.app'      # whatever started it, PID file or not
```

### Is it running?

```bash
ssh ich@192.168.1.197 'cd ~/roboface && ps -o pid=,etime=,cmd= -p "$(cat var/server.pid)"'
```
```
  20396       00:28 .venv/bin/python -m roboface_server.app
```

```bash
ssh ich@192.168.1.197 'ss -ltn | grep :8000'                                  # the listener
ssh ich@192.168.1.197 'ss -tn state established "( sport = :8000 )"'          # who is attached
```

`pgrep -af roboface_server.app` also works, but over SSH it matches **its own** wrapper: the
`bash -c` that runs it has the pattern in its command line. Filter it, or prefer the PID file:

```bash
ssh ich@192.168.1.197 'pgrep -af roboface_server.app | grep -v "bash -c"'
```

### Logs

```bash
ssh ich@192.168.1.197 'tail -n 50 ~/roboface/var/server.log'
ssh -t ich@192.168.1.197 'tail -f ~/roboface/var/server.log'    # -t so Ctrl-C reaches the tail
```

### Healthcheck, with no repository at all

```bash
curl -s -o /dev/null -m 5 http://192.168.1.197:8000/ -w '%{http_code}\n'
```

```
404   <- the application is answering. / is not a route; only /ws is, and it is a WebSocket route.
000   <- (curl exit 7) nothing is listening
```

**Any** HTTP status means the server is up: a 404 comes from the application, so it proves far more
than a bare TCP connect does. That distinction is the whole point — `nc -z` on this LAN has reported
"open" for a socket that was about to be destroyed unread.

For a check that also proves the *protocol* works, use `tools/remote.sh health`, which performs a
real `hello` + `ping`.

### Deploying without the script

```bash
rsync -az --delete --exclude '.git/' --exclude '.venv/' --exclude 'var/' --exclude 'server/.env' \
  ./ ich@192.168.1.197:roboface/
rsync -a server/.env ich@192.168.1.197:roboface/server/.env
ssh ich@192.168.1.197 'cd ~/roboface && VIRTUAL_ENV="$PWD/.venv" ~/.local/bin/uv pip install -r requirements.txt'
```

## What `deploy` does

1. **`rsync -az --delete`** of the working tree — so the box mirrors your checkout, including
   uncommitted work. Excluded: `.git/`, `.venv/`, caches, `codegen/runs/`, `codegen/var/`,
   `firmware/.pio/`, and `var/` (the remote's PID and log).
2. **`server/.env` separately**, at mode `600`. It is deliberately excluded from the tree sync:
   it holds the API keys, and a `--delete` run that happened to execute without it locally would
   otherwise remove the box's only copy.
3. **The toolchain**, idempotently — `uv` if absent, CPython 3.13, the venv, then
   `requirements.txt` (or `requirements-dev.txt` with `--dev`).

Re-running is cheap and safe; the running server is not restarted (use `restart` for that).

### Why a standalone Python and not the system one

Ubuntu 22.04 ships Python 3.10. The server targets 3.11+ (`pyproject.toml` sets
`target-version = "py311"`; CI runs 3.13). On 3.10 `asyncio.wait_for` raises `asyncio.TimeoutError`,
which is **not** the builtin `TimeoutError` — the alias arrived in 3.11. So
[orchestrator.py:130](server/roboface_server/orchestrator.py#L130)'s `except TimeoutError` would
quietly stop catching the first-token budget, and a stalled model would surface as an unhandled
exception instead of a clean `llm_timeout`. `tools/chat.py` has two more of these.

`uv` installs an interpreter under `$HOME`, so this needs no root and touches no system package.
The repos offer only a release *candidate* of 3.11 and nothing newer, which is the other reason not
to use apt here.

## Where things live on the box

| Path | What |
|---|---|
| `~/roboface/` | the deployed tree |
| `~/roboface/server/.env` | the API keys, mode `600` |
| `~/roboface/.venv/` | the virtualenv (CPython 3.13) |
| `~/roboface/var/server.pid` | the PID `start` wrote |
| `~/roboface/var/server.log` | stdout + stderr, appended across restarts |
| `~/.local/bin/uv`, `~/.local/share/uv/python/` | the toolchain |

`var/` is excluded from the sync, so redeploying never disturbs a running server's PID or log.
The log is append-only and nothing rotates it — truncate it if it grows: `tools/remote.sh shell`
then `: > var/server.log`.

## The server does not survive a reboot

`start` uses `nohup`, so the process outlives the SSH session but not the machine. After a reboot,
run `tools/remote.sh start` again.

To make it persistent, a systemd **user** unit needs lingering enabled, which is the one thing here
that requires `sudo` (once):

```bash
ssh ich@192.168.1.197 'sudo loginctl enable-linger $USER'
```

This was deliberately left out of the script: it is a one-time administrative decision about the
box, not part of deploying a build.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cannot ssh ... without a password` | key not installed | `ssh-copy-id $RF_REMOTE` |
| `stopped (no live pid file)` but a listener exists | a server started outside the script | `tools/remote.sh stop` names the stray PID; clear it with `pkill -f roboface_server.app` |
| `the process exited during startup` | bad `.env`, port taken, missing dependency | the last 15 log lines are printed with the error |
| `health` says **DEGRADED** | listening but not speaking the protocol | `logs -n 50` — usually a provider/key error |
| `health` says **DOWN**, `status` shows it running | the box's firewall | `ufw status` on the box; allow the port |
| board shows `[ws] reconnecting in …` | server down, or `SERVER_URL` points elsewhere | `health`, then check `firmware/src/config.h` |
| `pytest` on the box errors during collection | `tools/board.py` exits at import when pyserial is absent | known; see below |

> **Known defect.** `tools/board.py` borrows PlatformIO's pyserial and calls `raise SystemExit(1)`
> at import when it is missing. `tests/unit/test_board_tool.py` guards with `pytest.importorskip`,
> but that only catches `ImportError` — a `SystemExit` goes past it and takes pytest's whole
> collection down. So the suite does not run on a box without PlatformIO, even after
> `deploy --dev`. Run the suite on the workstation until this is fixed at the call site.

---

# Part 2 — The firmware

Built and flashed from the **workstation**, over USB. Run everything from `firmware/`.

## One-time configuration

`src/config.h` is **gitignored** — it holds the WiFi password — and must be created from the
template:

```bash
cd firmware
cp src/config.example.h src/config.h
$EDITOR src/config.h
```

| Define | Meaning |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | the network the **board** joins; must be the server's LAN |
| `SERVER_URL` | `ws://<server-ip>:8000/ws` — currently `ws://192.168.1.197:8000/ws` |
| `DEVICE_ID` | what the board announces in `hello` (`core-s3-01`) |
| `SCREEN_BRIGHTNESS` | 0–255 |

`SERVER_URL` is `ws://`, not `wss://`: the server runs uvicorn with no TLS. This is also why a
public tunnel is not an option for reaching it from outside the LAN — the firmware has no TLS.

**The board holds no model key.** `GEMINI_API_KEY` and the rest live only in `server/.env`.

## Build, flash, monitor

```bash
pio test -e native            # the pure half -- host-tested, no board needed
pio run  -e cores3            # compile for the board
pio run  -e cores3 -t upload  # flash it
```

The two environments are the architecture, not a convenience. `native` compiles **only**
`src/pure/` — header-only, `namespace roboface`, Arduino-free — so an `#include <M5Unified.h>`
that reaches pure code fails `pio test -e native` immediately, rather than being discovered by the
next person who tries to test that logic without hardware. `src/app/` is glue in `namespace app`,
validated by compiling plus the on-device checks.

To watch the board, use the repo's monitor rather than `pio device monitor` — the Core S3's USB is
the ESP32-S3's *native* USB, so the port re-enumerates on every reset and `pio device monitor` dies
at exactly the moment you want to be watching. `tools/board.py` reattaches:

```bash
.venv/bin/python tools/board.py                    # watch, and type commands at it
.venv/bin/python tools/board.py --for 40           # watch for 40s and exit
.venv/bin/python tools/board.py --send /faces      # drive the self-test from a script
.venv/bin/python tools/board.py --send /chat-on    # put the conversation on the panel
```

`/chat-on` turns the board into a chat console: what you type and the reply as it streams are drawn
where the face usually is, in a font that covers Ukrainian. `/chat-off` restores the face and the
device state it borrowed the screen from. See [TESTING.md](TESTING.md) §4 for what to look for.

## Pointing the board at a different server

`SERVER_URL` is compiled in, so this is an edit and a reflash:

```bash
cd firmware
sed -i '' 's#^\(#define[[:space:]]*SERVER_URL[[:space:]]*\).*#\1"ws://192.168.1.197:8000/ws"#' src/config.h
pio run -e cores3 -t upload
```

## A healthy boot

```
[status] wifi_connecting · link down 0.0.0.0 · ws disconnected · batt 100% · up 10s
[net] connecting to "Vitalii" (attempt 2)
[net] up, ip 192.168.1.131
[state] wifi_connecting
[ws] server 192.168.1.197:8000/ws
[ws] connected
[state] idle
[status] idle · link up 192.168.1.131 · ws connected · batt 100% · up 20s
```

`[ws] connected` followed by `[state] idle` is the link working. Confirm from the server side —
the `hello` carries the device's declared capabilities:

```bash
tools/remote.sh logs -n 30 | grep hello.negotiated
```
```json
{"event": "hello.negotiated", "device_id": "core-s3-01", "proto_ver": 1,
 "caps": ["camera", "dual_mic", "touch"], "audio_fmt": "pcm16/16000/1"}
```

`tools/remote.sh status` shows the same thing as a live session from `192.168.1.131`.

## Flashing gotchas

Both of these cost real time to learn.

**If a flash cannot connect** (`Device not configured`, or the port vanishing from `/dev`
mid-upload): a board still running M5Stack's UiFlow presents a USB CDC device that does not survive
esptool's reset handshake. Hold the power button ~2 s until the green LED lights to force the ROM
bootloader, then flash. Once this firmware is on the board the problem disappears — it uses the
hardware USB-Serial/JTAG controller (`303A:1001`), which handles the reset in silicon.

**The serial monitor drops on every reset** for the same reason: native USB re-enumerates, so the
port disappears and comes back as a new device. That is the board restarting, not a broken cable.
`tools/board.py` handles it by reattaching.

---

## Bringing the whole thing up from cold

```bash
tools/remote.sh deploy && tools/remote.sh start && tools/remote.sh health
cd firmware && pio run -e cores3 -t upload
cd .. && .venv/bin/python tools/board.py --for 40
tools/remote.sh status                      # the board should appear as a session
```

## See also

- [TESTING.md](TESTING.md) — the suites, and driving the server and the board
- [CLAUDE.md](CLAUDE.md) — repository conventions and the toolchain
- [specification/ARCHITECTURE.md](specification/ARCHITECTURE.md) — the contracts and the seams
