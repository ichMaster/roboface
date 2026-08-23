"""The bridge loop — subscribe to the dashboard, answer device polls.

A client of the dashboard, never a stage inside it (architecture §1.2). If this process
dies, the pipeline and the browser carry on: principle 2 extended one hop.

``--fake-device`` runs the whole thing end to end with no hardware, which is also how
it runs in CI. By the time that works, buying a board has stopped being a gamble.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import sys
from collections.abc import Sequence
from typing import Any

from bridge import devices, session, transport
from bridge.devices import Profile

#: The dashboard, which the bridge never imports — only talks to.
DEFAULT_WS = "ws://127.0.0.1:8420/ws"


class Bridge:
    """One dashboard subscription, N devices, each on its own schedule."""

    def __init__(self, profiles: Sequence[Profile], *, clock: Any = None) -> None:
        self._clock = clock or (lambda: asyncio.get_event_loop().time())
        self.sessions = {p.name: session.Session(p) for p in profiles}

    def observe(self, state: dict[str, Any]) -> None:
        """Hand a fresh snapshot to every device.

        Each keeps its own notification queue, so a device that has been out of range
        does not lose what happened while it was away — and one that just connected is
        not buzzed for all of it.
        """
        for sess in self.sessions.values():
            sess.observe(state)

    def answer_for(self, name: str) -> transport.Answer:
        async def answer(want: int, user: bool) -> dict[str, Any]:
            return self.sessions[name].answer(want, self._clock(), user=user)

        return answer

    async def consume(self, url: str) -> None:  # pragma: no cover - needs a server
        """Follow the dashboard, reconnecting for as long as it takes.

        A dashboard that is down must never crash the bridge: it is the thing being
        observed, and an observer that dies with its subject is not much of one.
        """
        import websockets

        delay = 1.0
        while True:
            try:
                async with websockets.connect(url) as ws:
                    delay = 1.0
                    async for raw in ws:
                        message = json.loads(raw)
                        state = message.get("state")
                        if isinstance(state, dict):
                            self.observe(state)
            except asyncio.CancelledError:
                raise
            except Exception:
                await asyncio.sleep(delay)
                delay = min(delay * 2, 30.0)


async def run_fake(bridge: Bridge, *, rounds: int = 20) -> list[str]:
    """Drive every device through a realistic poll schedule. Returns any guard failures.

    Exercises every screen and the notification channel on each device, so a frame that
    would not fit or would not render fails here rather than on a panel.
    """
    failures: list[str] = []
    for name, sess in bridge.sessions.items():
        fake = transport.FakeTransport(name)
        await fake.run(bridge.answer_for(name))
        wants = [devices.WANT_NOTIFY, *sess.profile.screens]
        for index in range(rounds):
            await fake.poll(wants[index % len(wants)], user=index % 7 == 0)
        failures.extend(fake.failed)
    return failures


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="bridge", description=__doc__)
    parser.add_argument("--url", default=DEFAULT_WS)
    parser.add_argument(
        "--fake-device", action="store_true",
        help="run end to end with no hardware; how this runs in CI",
    )
    parser.add_argument(
        "--profile", action="append", choices=sorted(devices.PROFILES),
        help="boards to serve; repeatable. Defaults to every board in the roster.",
    )
    args = parser.parse_args(argv)

    chosen = [devices.PROFILES[n] for n in (args.profile or sorted(devices.PROFILES))]
    bridge = Bridge(chosen)

    if args.fake_device:
        return _run_fake_once(bridge, args.url)

    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(_serve(bridge, args.url, chosen))
    return 0


def _run_fake_once(bridge: Bridge, url: str) -> int:
    state = _fetch_state(url)
    if state is None:
        print("no dashboard reachable; using an empty state", file=sys.stderr)
        state = {}
    bridge.observe(state)
    failures = asyncio.run(run_fake(bridge))
    for problem in failures:
        print(problem, file=sys.stderr)
    print(f"{len(bridge.sessions)} device(s), {len(failures)} guard failure(s)")
    return 1 if failures else 0


def _fetch_state(url: str) -> dict[str, Any] | None:
    """The dashboard's current state over HTTP, since ``--fake-device`` needs one
    snapshot rather than a subscription."""
    import urllib.error
    import urllib.request

    api = url.replace("ws://", "http://").replace("wss://", "https://").removesuffix("/ws")
    try:
        with urllib.request.urlopen(f"{api}/api/state", timeout=2) as response:
            result: dict[str, Any] = json.load(response)
            return result
    except (urllib.error.URLError, TimeoutError, ValueError, OSError):
        return None


async def _serve(  # pragma: no cover - needs a radio and a server
    bridge: Bridge, url: str, profiles: Sequence[Profile]
) -> None:
    """One dashboard subscription and one transport per device, all independent.

    Devices share no schedule — they ask when they are ready — so one dropping cannot
    stall another.
    """
    tasks = [asyncio.create_task(bridge.consume(url))]
    for profile in profiles:
        radio = transport.BleakTransport(profile.name)
        tasks.append(asyncio.create_task(radio.run(bridge.answer_for(profile.name))))
    await asyncio.gather(*tasks)


if __name__ == "__main__":  # pragma: no cover - process entry point
    raise SystemExit(main())
