"""A terminal chat client — a stand-in for the device, for testing RoboFace by hand.

The automated suite proves the server behaves; this is for the things a suite cannot show you:
what the streaming actually *feels* like, whether a reply reads well out loud, and what happens
when you type something odd at it. Until v0.3 puts firmware on a Core S3, this is the only way
to hold a conversation with the thing.

**It speaks the protocol through `protocol.py`**, exactly as `tests/fake_device.py` does and for
the same reason: a client that hand-rolls its own JSON becomes a second, divergent implementation
of the wire, and then a contract change breaks the device while this tool carries on looking
fine. Here a contract change breaks it loudly. The deliberate exceptions are `/raw`, `/bad` and
`/binary`, which exist precisely to send what the codec would refuse to produce.

Usage::

    # terminal 1
    PYTHONPATH=server .venv/bin/python -m roboface_server.app

    # terminal 2
    .venv/bin/python tools/chat.py

Type a line to say it. Type ``/help`` for the rest.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Final

# Run from a checkout without setting PYTHONPATH: the tool is meant to be typed at, not
# configured before use.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "server"))

from roboface_server.protocol import (  # noqa: E402 -- after the sys.path bootstrap above
    AUDIO_FMT,
    PROTO_VERSION,
    Capability,
    ErrorFrame,
    Frame,
    Hello,
    Ping,
    Reply,
    TextIn,
    decode,
    encode,
)

DEFAULT_URL: Final = "ws://127.0.0.1:8000/ws"

#: How long to wait for a turn to finish before giving up and handing the prompt back. Generous:
#: the first-token budget alone is ~8 s, and a slow-but-flowing reply is not a failure.
TURN_TIMEOUT_S: Final = 30.0


# ---------------------------------------------------------------------------------------
# Presentation — pure, so it can be tested without a socket
# ---------------------------------------------------------------------------------------


class Style:
    """ANSI codes, or nothing at all when the output is not a terminal."""

    def __init__(self, *, enabled: bool = True) -> None:
        self.enabled = enabled

    def _wrap(self, code: str, text: str) -> str:
        return f"\033[{code}m{text}\033[0m" if self.enabled else text

    def dim(self, text: str) -> str:
        return self._wrap("2", text)

    def bold(self, text: str) -> str:
        return self._wrap("1", text)

    def red(self, text: str) -> str:
        return self._wrap("31", text)

    def green(self, text: str) -> str:
        return self._wrap("32", text)

    def cyan(self, text: str) -> str:
        return self._wrap("36", text)


@dataclass(frozen=True, slots=True)
class Command:
    """A parsed input line: either something to say, or something to do."""

    kind: str
    argument: str = ""


def parse_input(line: str) -> Command:
    """Turn a typed line into a command.

    A leading ``/`` is a command; everything else is speech. ``//`` escapes, so a line that
    genuinely starts with a slash can still be said.
    """
    stripped = line.strip()
    if not stripped:
        return Command("blank")
    if stripped.startswith("//"):
        return Command("say", stripped[1:])
    if stripped.startswith("/"):
        head, _, rest = stripped[1:].partition(" ")
        return Command(head.lower() or "help", rest.strip())
    return Command("say", stripped)


def describe_frame(frame: Frame | str, style: Style) -> str:
    """How one server frame appears on screen.

    Unknown or not-yet-implemented frame types are rendered as raw JSON rather than dropped:
    when v1 adds ``asr_partial`` and v2 adds ``emotion``, this tool shows them the day they
    exist instead of the day someone remembers to teach it about them.
    """
    if isinstance(frame, str):
        return style.dim(f"   · {frame}")
    if isinstance(frame, ErrorFrame):
        return style.red(f"   ✗ {frame.code.value}: {frame.msg}")
    return style.dim(f"   · {type(frame).__name__.lower()}")


def format_timing(deltas: int, first_ms: float | None, total_ms: float, style: Style) -> str:
    """The line under a reply.

    Time-to-first-delta is the number this whole architecture exists to keep small — the point
    at which a person knows they have been heard — so it is shown next to the total rather than
    left to be inferred.
    """
    if first_ms is None:
        return style.dim(f"   ({total_ms:.0f} ms, no deltas)")
    return style.dim(
        f"   ({deltas} delta{'s' if deltas != 1 else ''} · "
        f"first {first_ms:.0f} ms · complete {total_ms:.0f} ms)"
    )


HELP = """
  <text>            say something
  /ping             send ping, expect pong
  /hello [ver]      re-send hello, optionally with a wrong proto_ver (expect proto_unsupported)
  /raw <json>       send text verbatim, bypassing the codec (expect bad_frame)
  /bad              send malformed JSON (expect bad_frame)
  /binary           send a binary frame, which carries no envelope (expect bad_frame)
  /stats            turns and timings so far
  /help             this
  /quit             leave
"""


# ---------------------------------------------------------------------------------------
# The session
# ---------------------------------------------------------------------------------------


@dataclass(slots=True)
class Stats:
    turns: int = 0
    deltas: int = 0
    errors: int = 0
    first_ms: list[float] = field(default_factory=list)

    def summary(self) -> str:
        if not self.turns:
            return "no turns yet"
        best = min(self.first_ms) if self.first_ms else 0.0
        worst = max(self.first_ms) if self.first_ms else 0.0
        mean = sum(self.first_ms) / len(self.first_ms) if self.first_ms else 0.0
        turns = f"{self.turns} turn{'s' if self.turns != 1 else ''}"
        return (
            f"{turns} · {self.deltas} deltas · {self.errors} errors · "
            f"first delta min {best:.0f} / mean {mean:.0f} / max {worst:.0f} ms"
        )


async def read_line(prompt: str) -> str:
    """Read stdin without blocking the event loop."""
    return await asyncio.to_thread(_blocking_read, prompt)


def _blocking_read(prompt: str) -> str:
    try:
        sys.stdout.write(prompt)
        sys.stdout.flush()
        return sys.stdin.readline()
    except (EOFError, KeyboardInterrupt):
        return ""


async def consume_turn(websocket: Any, style: Style, stats: Stats) -> None:
    """Render frames until the turn ends, showing each delta the moment it lands.

    Printing inside the loop rather than after it is the point: this tool exists partly to make
    the streaming visible, and a client that collected the deltas first would hide exactly the
    property the server works hardest to provide.
    """
    started = time.monotonic()
    first_ms: float | None = None
    deltas = 0
    printed_prefix = False

    while True:
        try:
            raw = await asyncio.wait_for(websocket.recv(), timeout=TURN_TIMEOUT_S)
        except TimeoutError:
            print(style.red(f"\n   ✗ no reply within {TURN_TIMEOUT_S:.0f}s"))
            return

        if isinstance(raw, bytes | bytearray):
            print(style.dim(f"   · binary frame, {len(raw)} bytes"))
            continue

        try:
            frame = decode(raw)
        except Exception:
            # Something the codec cannot read. Show it rather than crash -- if the server ever
            # sends this, seeing it is the whole point of being here.
            print(describe_frame(str(raw)[:200], style))
            continue

        if isinstance(frame, Reply):
            if frame.final:
                total_ms = (time.monotonic() - started) * 1000
                print()
                print(format_timing(deltas, first_ms, total_ms, style))
                stats.turns += 1
                stats.deltas += deltas
                if first_ms is not None:
                    stats.first_ms.append(first_ms)
                return
            if first_ms is None:
                first_ms = (time.monotonic() - started) * 1000
            if not printed_prefix:
                sys.stdout.write(style.green("   "))
                printed_prefix = True
            deltas += 1
            sys.stdout.write(frame.text)
            sys.stdout.flush()
            continue

        if isinstance(frame, ErrorFrame):
            if printed_prefix:
                print()
            print(describe_frame(frame, style))
            stats.errors += 1
            return

        print(describe_frame(frame, style))
        return


async def run(url: str, device_id: str, proto_ver: int, caps: frozenset[Capability]) -> int:
    from websockets.asyncio.client import connect
    from websockets.exceptions import ConnectionClosed

    style = Style(enabled=sys.stdout.isatty())
    stats = Stats()

    try:
        websocket = await connect(url)
    except OSError as exc:
        print(style.red(f"cannot reach {url}: {exc}"))
        print(style.dim("is the server running?  PYTHONPATH=server python -m roboface_server.app"))
        return 1

    async with websocket:
        await websocket.send(encode(Hello(device_id, proto_ver, AUDIO_FMT, caps)))
        print(style.bold(f"connected to {url}") + style.dim(f"  as {device_id}"))
        print(style.dim("/help for commands, /quit to leave"))

        # A rejected hello answers immediately; a good one says nothing, so peek briefly rather
        # than blocking the prompt behind a reply that is never coming.
        try:
            raw = await asyncio.wait_for(websocket.recv(), timeout=0.5)
            print(describe_frame(decode(raw), style))
            return 1
        except TimeoutError:
            pass
        print()

        while True:
            try:
                line = await read_line(style.cyan("you › "))
            except (KeyboardInterrupt, asyncio.CancelledError):
                print()
                break
            if not line:
                break

            command = parse_input(line)
            try:
                if command.kind in {"quit", "exit", "q"}:
                    break
                if command.kind == "blank":
                    continue
                if command.kind == "help":
                    print(style.dim(HELP))
                    continue
                if command.kind == "stats":
                    print(style.dim("   " + stats.summary()))
                    continue

                if command.kind == "say":
                    await websocket.send(encode(TextIn(command.argument)))
                elif command.kind == "ping":
                    await websocket.send(encode(Ping()))
                elif command.kind == "hello":
                    version = int(command.argument) if command.argument else proto_ver
                    await websocket.send(encode(Hello(device_id, version, AUDIO_FMT, caps)))
                elif command.kind == "raw":
                    await websocket.send(command.argument or "{}")
                elif command.kind == "bad":
                    await websocket.send("{not json at all")
                elif command.kind == "binary":
                    await websocket.send(b"\x00\x01\x02\x03")
                else:
                    print(style.dim(f"   unknown command /{command.kind} — try /help"))
                    continue

                await consume_turn(websocket, style, stats)
            except ConnectionClosed:
                print(style.red("   ✗ the server closed the connection"))
                break

    print(style.dim(stats.summary()))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Terminal chat client for the RoboFace server.")
    parser.add_argument("--url", default=DEFAULT_URL, help=f"websocket URL (default {DEFAULT_URL})")
    parser.add_argument("--device-id", default="terminal", help="the device_id to announce")
    parser.add_argument(
        "--proto-ver",
        type=int,
        default=PROTO_VERSION,
        help="protocol version to announce; a wrong one should be rejected",
    )
    parser.add_argument(
        "--caps",
        default="",
        help=f"comma-separated capabilities ({', '.join(c.value for c in Capability)})",
    )
    args = parser.parse_args()

    caps = frozenset(
        Capability(name.strip())
        for name in args.caps.split(",")
        if name.strip() in {c.value for c in Capability}
    )

    try:
        return asyncio.run(run(args.url, args.device_id, args.proto_ver, caps))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
