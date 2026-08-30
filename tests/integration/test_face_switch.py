"""v2.6 — the server changing the device's face, end to end.

**This file is the fix for a defect that every other kind of test missed.** `ConfigUpdated` had a
contract test, round-tripped through the codec, was mirrored in the firmware and was parsed by the
device — and nothing on the server ever sent one. The roadmap's DoD is that the face is *"switchable
both ways"*; only one of those ways existed.

That is the third time this project has shipped the shape: `TouchGestures::reset()` in v2.4 (written
for a documented case, never called), `frame_for(gaze=…)` in v2.5 (a parameter with no caller), and
this. A test of a thing's behaviour cannot see that nothing invokes it — so these tests start from
the outside, at an HTTP request, and assert that a frame reaches a device.
"""

from __future__ import annotations

from fake_device import connect
from fastapi import FastAPI
from fastapi.testclient import TestClient
from roboface_server.app import create_app
from roboface_server.protocol import ConfigUpdated
from roboface_server.router import ConnectionRegistry


def _app() -> tuple[FastAPI, ConnectionRegistry]:
    registry = ConnectionRegistry()
    return create_app(registry=registry), registry


def test_the_endpoint_reaches_a_connected_device() -> None:
    app, _ = _app()
    with connect(app) as device:
        device.hello()

        with TestClient(app) as http:
            response = http.post("/face/ghost")

        assert response.status_code == 200
        assert response.json() == {"face_set": "ghost", "devices": 1}
        assert device.recv_any() == ConfigUpdated(face_set="ghost")


def test_an_unknown_face_is_a_404_naming_the_ones_that_exist() -> None:
    """A typo must not look like a switch that worked.

    The message lists the real names because the person making the request is at a terminal and the
    device is in another room — there is nowhere else for them to find out.
    """
    app, _ = _app()
    with connect(app) as device:
        device.hello()

        with TestClient(app) as http:
            response = http.post("/face/dragon")

        assert response.status_code == 404
        assert "ghost" in response.json()["detail"]


def test_with_nothing_connected_it_says_zero_rather_than_failing() -> None:
    """**A count, not success or failure.**

    "Nothing is connected" is a perfectly ordinary state — the board is unplugged, or rebooting —
    and it is a different fact from "the face_set was wrong". Reporting it as an error would send
    someone looking for a bug in the frame.
    """
    app, _ = _app()
    with TestClient(app) as http:
        response = http.post("/face/flame")

    assert response.status_code == 200
    assert response.json()["devices"] == 0


def test_every_declared_face_can_be_asked_for() -> None:
    from roboface_server.protocol import FACE_SETS

    app, _ = _app()
    with connect(app) as device:
        device.hello()

        with TestClient(app) as http:
            for face_set in sorted(FACE_SETS):
                assert http.post(f"/face/{face_set}").status_code == 200
                assert device.recv_any() == ConfigUpdated(face_set=face_set)
