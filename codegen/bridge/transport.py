"""Transports — one interface, two implementations.

The fake is not a lesser version of the real one. It is what makes the poll loop,
the routing, the pacing and the guards testable in milliseconds without a radio, a
board, or a purchase — and by the time it works, buying hardware has stopped being a
gamble (M5-010).

``BleakTransport`` is deliberately thin. Everything worth getting wrong already lives
in ``session`` and ``project``, both of which are pure; what is left here is scanning,
connecting, and two GATT characteristics.
"""

from __future__ import annotations

import asyncio
import json
from collections.abc import Awaitable, Callable
from typing import Any, Protocol

from bridge import frames

#: Called with ``(want, user)`` when a device asks for something. Returns the frame.
Answer = Callable[[int, bool], Awaitable[dict[str, Any]]]


class Transport(Protocol):
    """What the loop needs from a device, and nothing more."""

    name: str

    async def run(self, answer: Answer) -> None:
        """Serve polls until cancelled."""


def decode_request(payload: bytes | str) -> tuple[int, bool] | None:
    """Parse ``{"want":N}`` or ``{"want":N,"u":1}``.

    Returns ``None`` for anything unparseable or out of range, so the loop can ignore
    it rather than answer with a malformed frame — an unknown ``want`` has no frame to
    be the answer to.
    """
    try:
        message = json.loads(payload)
    except (ValueError, TypeError):
        return None
    if not isinstance(message, dict):
        return None
    want = message.get("want")
    if not isinstance(want, int) or isinstance(want, bool) or want not in frames.REQUIRED:
        return None
    return want, bool(message.get("u"))


class FakeTransport:
    """Records every write and lets a test drive polls synchronously.

    Two of these cover the case that matters at M5-020: one device dropping must not
    stall the other.
    """

    def __init__(self, name: str = "fake") -> None:
        self.name = name
        self.writes: list[dict[str, Any]] = []
        self.failed: list[str] = []
        self._answer: Answer | None = None

    async def run(self, answer: Answer) -> None:
        self._answer = answer
        # Nothing to serve on its own: a test drives it through :meth:`poll`.
        await asyncio.sleep(0)

    async def poll(self, want: int, *, user: bool = False) -> dict[str, Any]:
        """Ask, as a device would, and record what comes back."""
        assert self._answer is not None, "run() must be awaited before polling"
        frame = await self._answer(want, user)
        problems = frames.validate(frame)
        if problems:
            self.failed.append(f"{self.name} want={want}: {problems[0]}")
        self.writes.append(frame)
        return frame

    async def send(self, raw: bytes | str) -> dict[str, Any] | None:
        """Poll from a raw request, exercising the same decode path as the radio."""
        request = decode_request(raw)
        if request is None:
            return None
        return await self.poll(request[0], user=request[1])


class BleakTransport:
    """The real radio. Substitutes for the fake behind the same interface.

    ``bleak`` is imported inside ``run`` rather than at module scope so that importing
    ``bridge`` — which the test suite does constantly — never requires it. The package
    is a leaf and the dashboard must keep starting on a machine that has none of this.
    """

    #: 128-bit UUIDs, generated once and fixed. The firmware advertises the service and
    #: the bridge finds it by that rather than by device name, which a user can change.
    SERVICE_UUID = "6d356767-0001-4a6d-9d3a-5f5f6d357374"
    FRAME_UUID = "6d356767-0002-4a6d-9d3a-5f5f6d357374"
    INPUT_UUID = "6d356767-0003-4a6d-9d3a-5f5f6d357374"

    def __init__(self, name: str, *, address: str | None = None,
                 backoff_s: float = 1.0, max_backoff_s: float = 30.0) -> None:
        self.name = name
        self.address = address
        self.backoff_s = backoff_s
        self.max_backoff_s = max_backoff_s

    async def run(self, answer: Answer) -> None:  # pragma: no cover - needs a radio
        """Connect, serve, reconnect. Never raises out of the loop.

        A device out of range is the normal state, not an error: the bridge starts and
        stays healthy with no device present at all.
        """
        from bleak import BleakClient, BleakScanner

        delay = self.backoff_s
        while True:
            try:
                device = await BleakScanner.find_device_by_filter(
                    lambda d, ad: self.SERVICE_UUID in (ad.service_uuids or []),
                    timeout=10.0,
                )
                if device is None:
                    await asyncio.sleep(delay)
                    delay = min(delay * 2, self.max_backoff_s)
                    continue

                async with BleakClient(device) as client:
                    delay = self.backoff_s

                    async def on_input(_: Any, data: bytearray) -> None:
                        request = decode_request(bytes(data))
                        if request is None:
                            return
                        frame = await answer(request[0], request[1])
                        await client.write_gatt_char(
                            self.FRAME_UUID, frames.encode(frame), response=False
                        )

                    await client.start_notify(self.INPUT_UUID, on_input)
                    while client.is_connected:
                        await asyncio.sleep(1.0)
            except asyncio.CancelledError:
                raise
            except Exception:
                await asyncio.sleep(delay)
                delay = min(delay * 2, self.max_backoff_s)
