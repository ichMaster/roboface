"""Serial monitor for the board — survives the Core S3 re-enumerating.

`pio device monitor` opens the port once and dies the first time the board restarts. On the Core S3
that happens constantly: its USB-C is the ESP32-S3's *native* USB, so the port disappears from
/dev on every reset and comes back as a new device. A monitor that cannot survive that is one that
dies exactly when you most want to be watching — at boot, after a flash, or when the thing you are
debugging reboots.

This reattaches. It also sends lines, so the firmware's serial commands (`/faces`, `/debug`,
`/help`) can be driven from a script rather than typed.

Usage::

    .venv/bin/python tools/board.py                       # watch until Ctrl-C
    .venv/bin/python tools/board.py --for 30              # watch for 30 seconds
    .venv/bin/python tools/board.py --send /faces         # watch, and send once attached
    .venv/bin/python tools/board.py --send /faces --for 25 --quiet

Run it from anywhere; it finds its own way to the board.
"""

from __future__ import annotations

import argparse
import glob
import sys
import time
from contextlib import suppress
from typing import Any

# PlatformIO ships pyserial and the project venv does not, so borrow it rather than adding a
# dependency the product does not need. Falls back to whatever `serial` is importable.
_PIO_SITE_PACKAGES = sorted(
    glob.glob("/opt/homebrew/Cellar/platformio/*/libexec/lib/python*/site-packages")
    + glob.glob("/usr/local/Cellar/platformio/*/libexec/lib/python*/site-packages")
)
for path in _PIO_SITE_PACKAGES:
    if path not in sys.path:
        sys.path.append(path)

try:
    # No stubs ship for pyserial, and it is a dev tool borrowing PlatformIO's copy rather than a
    # declared dependency -- so the import is untyped by construction, not by oversight.
    import serial  # type: ignore[import-untyped]  # noqa: E402 -- after the sys.path fix-up
except ImportError:  # pragma: no cover -- environment problem, not a code path
    print(
        "pyserial not found. It ships with PlatformIO; install it with:\n"
        "    .venv/bin/pip install pyserial",
        file=sys.stderr,
    )
    raise SystemExit(1) from None

#: The Core S3 appears here. Two different ids in practice -- one for the running firmware, one for
#: the ROM bootloader -- so match the family rather than a fixed name.
PORT_GLOB = "/dev/cu.usbmodem*"
BAUD = 115200


def find_port() -> str | None:
    """The board's serial port, or None while it is between enumerations."""
    ports = sorted(glob.glob(PORT_GLOB))
    return ports[0] if ports else None


def split_lines(buffer: bytes) -> tuple[list[str], bytes]:
    """Complete lines out of a byte buffer, and whatever is left over.

    Pure, so the awkward part -- a read that lands mid-line, which is most of them -- is testable
    without a board attached.
    """
    lines: list[str] = []
    while b"\n" in buffer:
        line, buffer = buffer.split(b"\n", 1)
        lines.append(line.decode("utf-8", "replace").rstrip("\r"))
    return lines, buffer


def watch(duration: float | None, to_send: list[str], send_after: float, quiet: bool) -> int:
    started = time.time()
    deadline = None if duration is None else started + duration

    connection: Any = None
    port: str | None = None
    buffer = b""
    sent = False

    def note(message: str) -> None:
        if not quiet:
            print(f"[board] {message}", flush=True)

    note(f"watching {PORT_GLOB} — Ctrl-C to stop")
    try:
        while deadline is None or time.time() < deadline:
            if connection is None:
                found = find_port()
                if found is None:
                    time.sleep(0.2)
                    continue
                try:
                    connection = serial.Serial(found, BAUD, timeout=0.2)
                    port = found
                    note(f"attached to {port}")
                except OSError:
                    # The port can vanish between the glob and the open; that is the board
                    # rebooting, not an error worth reporting.
                    connection = None
                    time.sleep(0.3)
                    continue

            try:
                data = connection.read(4096)
            except Exception:
                note("port dropped — the board is resetting; waiting for it to come back")
                with suppress(Exception):
                    connection.close()
                connection = None
                buffer = b""
                time.sleep(0.5)
                continue

            if data:
                buffer += data
                lines, buffer = split_lines(buffer)
                for line in lines:
                    print(line, flush=True)

            if to_send and not sent and time.time() - started >= send_after:
                for text in to_send:
                    note(f">>> {text}")
                    try:
                        connection.write((text + "\n").encode())
                    except Exception as exc:
                        note(f"could not send: {exc}")
                    time.sleep(0.4)
                sent = True
    except KeyboardInterrupt:
        print(flush=True)
    finally:
        if connection is not None:
            if buffer:
                print(buffer.decode("utf-8", "replace"), flush=True)
            connection.close()

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--for", dest="duration", type=float, default=None,
                        help="seconds to watch (default: until Ctrl-C)")
    parser.add_argument("--send", action="append", default=[], metavar="LINE",
                        help="a line to type at the board once attached; repeatable")
    parser.add_argument("--send-after", type=float, default=2.0,
                        help="seconds to wait before sending (default 2)")
    parser.add_argument("--quiet", action="store_true", help="only the board's own output")
    args = parser.parse_args()

    if find_port() is None:
        print(f"No board found at {PORT_GLOB}. Is it plugged in?", file=sys.stderr)
        # Not a hard failure: the board may appear in a moment, and --for lets you wait for it.
        if args.duration is None:
            return 1

    return watch(args.duration, args.send, args.send_after, args.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
