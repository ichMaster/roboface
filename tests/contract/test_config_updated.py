"""v2.6 — `config_updated{face_set}` and the face a device reports in `hello`.

The pinned seam here is small and unusually consequential: it is the only frame where the **server
tells the device what to look like**, and the only field on `hello` that describes the device's
appearance rather than its capabilities. Both directions of a disagreement are silent by nature —
an unknown name that is quietly ignored looks exactly like a switch that worked — so both are
refused loudly instead.
"""

from __future__ import annotations

import json

import pytest
from roboface_server.protocol import (
    FACE_SETS,
    Capability,
    ConfigUpdated,
    Hello,
    MalformedFrame,
    decode,
    encode,
)


def test_config_updated_round_trips() -> None:
    payload = json.loads(encode(ConfigUpdated(face_set="ghost")))

    assert payload == {"type": "config_updated", "face_set": "ghost"}
    assert decode(encode(ConfigUpdated(face_set="ghost"))) == ConfigUpdated(face_set="ghost")


@pytest.mark.parametrize("face_set", sorted(FACE_SETS))
def test_every_declared_face_round_trips(face_set: str) -> None:
    assert decode(encode(ConfigUpdated(face_set=face_set))).face_set == face_set


def test_an_unknown_face_is_refused_rather_than_accepted() -> None:
    """**Refused, not coerced**, and the contrast with `emotion{}` is the point.

    A bad emotion becomes `neutral` and is rendered, because a face is not worth dropping a
    connection over. A bad `face_set` is a *disagreement about what faces exist* — the two sides
    were built from different vocabularies — and accepting it would leave the server believing the
    device wears a skin it has never heard of.
    """
    with pytest.raises(MalformedFrame, match="face_set"):
        decode(json.dumps({"type": "config_updated", "face_set": "dragon"}))

    with pytest.raises(MalformedFrame):
        decode(json.dumps({"type": "config_updated"}))


def test_hello_carries_the_face_the_device_is_wearing() -> None:
    hello = Hello(
        device_id="rf-1",
        proto_ver=1,
        audio_fmt="pcm16/16000/1",
        caps=frozenset({Capability.TOUCH}),
        face_set="flame",
    )
    payload = json.loads(encode(hello))

    assert payload["face_set"] == "flame"
    assert decode(encode(hello)).face_set == "flame"


def test_a_device_without_skins_omits_the_field_rather_than_sending_null() -> None:
    """A field always present and usually meaningless teaches every reader to skip it.

    It also keeps a pre-v2.6 firmware valid: `hello` without `face_set` is a device that has no
    opinion, not a device claiming to be faceless.
    """
    hello = Hello(device_id="rf-1", proto_ver=1, audio_fmt="pcm16/16000/1", caps=frozenset())
    payload = json.loads(encode(hello))

    assert "face_set" not in payload
    assert decode(encode(hello)).face_set is None


def test_a_hello_claiming_an_unknown_face_is_refused() -> None:
    """The device reporting a fact about itself, validated the way `event{}` is.

    A firmware typo here would otherwise look exactly like a working switch to a face the server
    has never heard of.
    """
    with pytest.raises(MalformedFrame, match="face_set"):
        decode(
            json.dumps(
                {
                    "type": "hello",
                    "device_id": "rf-1",
                    "proto_ver": 1,
                    "audio_fmt": "pcm16/16000/1",
                    "caps": [],
                    "face_set": "dragon",
                }
            )
        )

    with pytest.raises(MalformedFrame, match="face_set"):
        decode(
            json.dumps(
                {
                    "type": "hello",
                    "device_id": "rf-1",
                    "proto_ver": 1,
                    "audio_fmt": "pcm16/16000/1",
                    "caps": [],
                    "face_set": 7,
                }
            )
        )
