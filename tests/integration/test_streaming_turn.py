"""The v0.2 DoD, asserted end to end through the real ASGI application.

Roadmap §v0.2: a `text_in` returns an answer *"arriving as a stream of deltas, first delta well
before the reply is complete"*. That sentence has two halves and the second is the one tests
usually skip: counting frames proves the reply was **split**, not that it was **streamed**. A
server that generated the whole reply and then sent it in five pieces would pass a counting
test and fail every latency promise built on top of it.

So the tests here measure *when* deltas arrive relative to the provider still working.
"""

from __future__ import annotations

import time

import pytest
from fake_device import connect
from fastapi import FastAPI
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import ErrorCode, ErrorFrame, Reply, TextIn
from roboface_server.providers import MockLLMProvider, ProviderError, SilentLLMProvider
from roboface_server.router import ConnectionRegistry

#: Long enough to measure reliably on a loaded CI runner, short enough to keep the suite quick.
STALL_S = 0.4


def _app(provider: object, registry: ConnectionRegistry | None = None, **kwargs: object) -> FastAPI:
    orchestrator = Orchestrator(provider=provider, **kwargs)  # type: ignore[arg-type]
    return create_app(responder=orchestrator, registry=registry)


def test_a_turn_arrives_as_many_reply_frames() -> None:
    app = _app(MockLLMProvider(deltas=["При", "віт", ", ", "друже", "!"]))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        reply = device.collect_reply()

    assert reply.deltas == ("При", "віт", ", ", "друже", "!")
    assert reply.text == "Привіт, друже!"
    assert len(reply) == 5


def test_deltas_arrive_while_the_model_is_still_generating() -> None:
    """The DoD's real claim, and the one a frame count cannot make.

    The provider stalls before its fourth delta. If the server streams, the first three cross
    the wire before that stall begins; if it buffers, nothing arrives until the whole reply is
    done and the first delta is as late as the last.
    """
    provider = MockLLMProvider(
        deltas=["a", "b", "c", "d", "e"],
        delay_s=STALL_S,
        delay_before_index=3,
    )
    app = _app(provider)

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))

        started = time.monotonic()
        early = device.drain(3)
        early_elapsed = time.monotonic() - started

        rest = device.collect_reply()
        total_elapsed = time.monotonic() - started

    assert [frame.text for frame in early] == ["a", "b", "c"]  # type: ignore[union-attr]
    assert early_elapsed < STALL_S, (
        f"the first three deltas took {early_elapsed:.3f}s, which is the whole stall -- "
        "the reply was buffered, not streamed"
    )
    assert total_elapsed >= STALL_S, "the stall did not happen; the test proved nothing"
    assert rest.deltas == ("d", "e")


def test_the_terminal_frame_closes_the_turn_without_repeating_it() -> None:
    app = _app(MockLLMProvider(deltas=["один", "два"]))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="x"))
        frames = device.drain(3)

    assert frames == [
        Reply(text="один", final=False),
        Reply(text="два", final=False),
        Reply(text="", final=True),
    ]


def test_two_turns_on_one_connection_share_a_history() -> None:
    provider = MockLLMProvider(deltas=["ok"])
    app = _app(provider)

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="перше"))
        device.collect_reply()
        device.send(TextIn(text="друге"))
        device.collect_reply()

    _, messages = provider.calls[1]
    assert [message.text for message in messages] == ["перше", "ok", "друге"]


def test_two_devices_do_not_share_a_history() -> None:
    provider = MockLLMProvider(deltas=["ok"])
    app = _app(provider)

    with connect(app, device_id="a") as first, connect(app, device_id="b") as second:
        first.hello()
        first.send(TextIn(text="from a"))
        first.collect_reply()

        second.hello()
        second.send(TextIn(text="from b"))
        second.collect_reply()

    _, second_call = provider.calls[1]
    assert [message.text for message in second_call] == ["from b"], (
        "the second device saw the first device's conversation"
    )


def test_a_stalled_model_fails_fast_with_llm_timeout() -> None:
    provider = MockLLMProvider(delay_s=5.0, delay_before_index=0)
    app = _app(provider, first_token_budget_s=0.05)

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))

        error = device.recv_until(ErrorFrame)

    assert error.code is ErrorCode.LLM_TIMEOUT


def test_a_silent_model_ends_the_turn_rather_than_erroring() -> None:
    app = _app(SilentLLMProvider())

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        reply = device.collect_reply()

    assert reply.text == ""
    assert reply.deltas == ()


def test_a_mid_stream_failure_sends_an_error_and_no_terminal_reply() -> None:
    provider = MockLLMProvider(
        deltas=["почина", "ю відпов", "ідати"],
        fail_at_index=2,
        error=ProviderError("the model died mid-sentence"),
    )
    app = _app(provider)

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))

        frames = device.drain(3)

    assert frames[0] == Reply(text="почина", final=False)
    assert frames[1] == Reply(text="ю відпов", final=False)
    assert isinstance(frames[2], ErrorFrame)
    assert frames[2].code is ErrorCode.LLM_FAILED
    # No `final: true` -- it would present half a sentence as a finished answer.
    assert not any(isinstance(frame, Reply) and frame.final for frame in frames)


def test_the_connection_survives_a_failed_turn() -> None:
    provider = MockLLMProvider(deltas=["a", "b"], fail_at_index=1)
    app = _app(provider)

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="this one fails"))
        device.recv_until(ErrorFrame)

        # The session stays connected (ARCHITECTURE §Budgets and abort semantics); only the
        # turn was lost. Repoint the mock at a working stream and try again.
        provider.fail_at_index = None
        device.send(TextIn(text="this one works"))

        assert device.collect_reply().text == "ab"


@pytest.mark.parametrize("text", ["", "привіт", "x" * 2000])
def test_awkward_input_still_streams(text: str) -> None:
    app = _app(MockLLMProvider(deltas=["ok"]))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text=text))

        assert device.collect_reply().text == "ok"


# ---------------------------------------------------------------------------------------
# The device's view of a failure (RF-009)
# ---------------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("raw", "why"),
    [
        ("{not json at all", "malformed JSON"),
        ('{"type": "sing_a_song"}', "an unknown message type"),
        ('{"type": "emotion"}', "a declared type this version does not implement"),
        ('{"type": "text_in"}', "a required field missing"),
    ],
)
def test_a_devices_own_bad_frame_is_reported_as_bad_frame(raw: str, why: str) -> None:
    """v0.1 reported all of these as `internal` — the server blaming itself for the device.

    From v0.4 the device renders the error face for whatever code it is sent, so the
    attribution is not cosmetic: it decides which side a person is told is at fault.
    """
    app = _app(MockLLMProvider(deltas=["ok"]))

    with connect(app) as device:
        device.hello()
        device.send_raw(raw)

        error = device.recv_until(ErrorFrame)

    assert error.code is ErrorCode.BAD_FRAME, f"{why} should be the device's fault"


def test_a_provider_failure_is_not_reported_as_bad_frame() -> None:
    # The other direction of the same distinction: the device's frame was fine.
    app = _app(MockLLMProvider(fail_at_index=0, error=ProviderError("model down")))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="a perfectly good question"))

        error = device.recv_until(ErrorFrame)

    assert error.code is ErrorCode.LLM_FAILED


def test_a_failed_turn_does_not_poison_the_next_one() -> None:
    """History rollback, observed from outside.

    Without it the model sees, on the next turn, a question it never answered — and often
    answers *that* instead of the one just asked, which reads as the character ignoring you.
    """
    provider = MockLLMProvider(deltas=["добре"], fail_at_index=0)
    app = _app(provider)

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="цей провалиться"))
        device.recv_until(ErrorFrame)

        provider.fail_at_index = None
        device.send(TextIn(text="цей спрацює"))
        device.collect_reply()

    _, messages = provider.calls[-1]
    assert [message.text for message in messages] == ["цей спрацює"]


def test_a_provider_outage_does_not_tell_the_device_the_server_is_unreachable() -> None:
    """Code review #3, from the device's side.

    DEVICE_UI renders a fault as the enumerated code verbatim — "No server ·
    `server_unreachable`". A Gemini outage must not produce that on a device that is
    connected to its server and being told so over that connection.
    """

    class _Outage(Exception):
        code = 503

    app = _app(MockLLMProvider(fail_at_index=0, error=ProviderError("upstream 503")))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))

        error = device.recv_until(ErrorFrame)

    assert error.code is ErrorCode.LLM_FAILED
    assert error.code is not ErrorCode.SERVER_UNREACHABLE
    assert error.code is not ErrorCode.UNAUTHORIZED
