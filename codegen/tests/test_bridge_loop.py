"""M5-009 / M5-010 / M5-011 — the poll loop, the fake, and the radio.

Everything worth getting wrong lives in ``session`` and ``project``, both pure and
already tested. What is left here is routing, decoding, and the end-to-end run — all of
which the fake covers in milliseconds, with no radio and no purchase.
"""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime
from typing import Any

import pytest

from bridge import devices, frames, main, notify, transport
from tests import gen_log
from tracker import reduce as reduce_mod

NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)


def _state() -> dict[str, Any]:
    return reduce_mod.reduce(gen_log.preset("clean-run").splitlines(), NOW).as_dict()


def _bridge(*profiles: devices.Profile) -> main.Bridge:
    clock = iter(range(10_000))
    bridge = main.Bridge(profiles or (devices.CORE2,), clock=lambda: float(next(clock)))
    bridge.observe(_state())
    return bridge


async def _fake(bridge: main.Bridge, name: str) -> transport.FakeTransport:
    fake = transport.FakeTransport(name)
    await fake.run(bridge.answer_for(name))
    return fake


# ── decoding ─────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("raw", ['{"want":4}', b'{"want":4}', '{"want":4,"u":0}'])
def test_a_well_formed_request_decodes(raw: bytes | str) -> None:
    assert transport.decode_request(raw) == (4, False)


def test_the_user_flag_survives_decoding() -> None:
    assert transport.decode_request('{"want":1,"u":1}') == (1, True)


@pytest.mark.parametrize(
    "raw",
    ["", "not json", "[]", '{"want":99}', '{"want":"1"}', '{"want":true}', '{"nope":1}'],
)
def test_an_unusable_request_decodes_to_nothing(raw: str) -> None:
    """Ignored rather than answered: an unknown ``want`` has no frame to be the answer
    to, and inventing one would put a malformed frame on the wire."""
    assert transport.decode_request(raw) is None


# ── the loop ─────────────────────────────────────────────────────────────────


def test_a_poll_produces_exactly_one_valid_frame_for_that_screen() -> None:
    async def go() -> None:
        bridge = _bridge()
        fake = await _fake(bridge, devices.CORE2.name)
        frame = await fake.poll(devices.SCREEN_PLAN)
        assert frame["s"] == devices.SCREEN_PLAN
        assert len(fake.writes) == 1
        assert frames.validate(frame) == []

    asyncio.run(go())


def test_an_unknown_want_is_ignored_rather_than_answered() -> None:
    async def go() -> None:
        bridge = _bridge()
        fake = await _fake(bridge, devices.CORE2.name)
        assert await fake.send('{"want":99}') is None
        assert fake.writes == []

    asyncio.run(go())


def test_every_write_of_a_full_run_passes_both_guards() -> None:
    """Asserted over the whole run rather than sampled — a frame that only fails on
    one screen at one moment is exactly the one sampling misses."""
    async def go() -> None:
        bridge = _bridge(devices.CORE2, devices.STICKC)
        failures = await main.run_fake(bridge, rounds=40)
        assert failures == []

    asyncio.run(go())


def test_the_fake_run_exercises_every_screen_and_the_notification_channel() -> None:
    async def go() -> None:
        bridge = _bridge()
        fake = await _fake(bridge, devices.CORE2.name)
        wants = [devices.WANT_NOTIFY, *devices.CORE2.screens]
        for want in wants:
            await fake.poll(want)
        assert {f["s"] for f in fake.writes} == set(wants)

    asyncio.run(go())


def test_a_second_device_is_served_independently() -> None:
    """Devices share no schedule — they ask when they are ready — so one dropping
    cannot stall another. Two fakes are what covers that."""
    async def go() -> None:
        bridge = _bridge(devices.CORE2, devices.STICKC)
        core2 = await _fake(bridge, devices.CORE2.name)
        stickc = await _fake(bridge, devices.STICKC.name)

        await core2.poll(devices.SCREEN_BURNDOWN)
        await stickc.poll(devices.SCREEN_NOW)
        await core2.poll(devices.SCREEN_NOW)

        assert len(core2.writes) == 2
        assert len(stickc.writes) == 1
        assert core2.failed == [] and stickc.failed == []

    asyncio.run(go())


def test_each_device_keeps_its_own_notification_queue() -> None:
    """One that has been out of range must not lose what happened while it was away."""
    async def go() -> None:
        bridge = _bridge(devices.CORE2, devices.STICKC)
        for sess in bridge.sessions.values():
            sess.queue._pending = [notify.Notification("retry", "RF-007 x2")]

        core2 = await _fake(bridge, devices.CORE2.name)
        await core2.poll(devices.WANT_NOTIFY)
        assert bridge.sessions[devices.CORE2.name].queue.peek() == []
        assert bridge.sessions[devices.STICKC.name].queue.peek() != []

    asyncio.run(go())


def test_polls_arriving_before_any_state_still_get_a_valid_frame() -> None:
    """Answering with something honest beats answering with nothing: a device asking
    before the dashboard has spoken is the normal case at startup."""
    async def go() -> None:
        bridge = main.Bridge([devices.CORE2], clock=lambda: 0.0)
        fake = await _fake(bridge, devices.CORE2.name)
        frame = await fake.poll(devices.SCREEN_NOW)
        assert frames.validate(frame) == []
        assert frame["idit"] == "0/0"

    asyncio.run(go())


# ── the CLI ──────────────────────────────────────────────────────────────────


def test_fake_device_runs_end_to_end_with_no_hardware(capsys: pytest.CaptureFixture[str]) -> None:
    """The point at which buying a board stops being a gamble."""
    assert main.main(["--fake-device", "--url", "ws://127.0.0.1:1/ws"]) == 0
    out = capsys.readouterr()
    assert "0 guard failure(s)" in out.out
    assert "no dashboard reachable" in out.err


def test_fake_device_can_be_pointed_at_one_board(capsys: pytest.CaptureFixture[str]) -> None:
    assert main.main(["--fake-device", "--profile", "stickc", "--url", "ws://127.0.0.1:1/ws"]) == 0
    assert "1 device(s)" in capsys.readouterr().out


def test_fake_device_exits_nonzero_when_a_guard_fails(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """It must fail CI, not merely mention the problem."""
    async def broken(bridge: main.Bridge, rounds: int = 20) -> list[str]:
        return ["core2 want=1: frame is 500 B, over the 182 B limit"]

    monkeypatch.setattr(main, "run_fake", broken)
    assert main.main(["--fake-device", "--url", "ws://127.0.0.1:1/ws"]) == 1
    assert "over the 182 B limit" in capsys.readouterr().err


def test_a_missing_dashboard_is_survivable_rather_than_fatal() -> None:
    """The bridge observes the pipeline; an observer that dies with its subject is not
    much of one."""
    assert main._fetch_state("ws://127.0.0.1:1/ws") is None


# ── the radio, as far as it goes without one ─────────────────────────────────


def test_the_real_transport_satisfies_the_same_interface() -> None:
    """Substituting it for the fake requires no change in the loop."""
    radio = transport.BleakTransport("core2")
    assert hasattr(radio, "run") and radio.name == "core2"


def test_bleak_is_not_imported_merely_by_importing_the_bridge() -> None:
    """The package is a leaf; the dashboard must keep starting on a machine with no
    Bluetooth and no ``bleak`` installed."""
    import pathlib

    source = pathlib.Path(transport.__file__).read_text()
    module_level = source.split("class BleakTransport")[0]
    assert "import bleak" not in module_level
    assert "from bleak" not in module_level


def test_the_uuids_are_fixed_and_distinct() -> None:
    """The bridge finds the board by service UUID rather than by name, which a user
    can change."""
    uuids = {
        transport.BleakTransport.SERVICE_UUID,
        transport.BleakTransport.FRAME_UUID,
        transport.BleakTransport.INPUT_UUID,
    }
    assert len(uuids) == 3
