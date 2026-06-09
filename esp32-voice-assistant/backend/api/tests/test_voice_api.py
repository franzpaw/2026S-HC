import asyncio
import base64
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from siri_backend.app import MAX_UPLOAD_BYTES, app
from siri_backend.fixtures import make_dummy_wav


client = TestClient(app)


def parse_sse_events(response_text: str) -> list[dict[str, object]]:
    events = []
    for block in response_text.strip().split("\n\n"):
        lines = block.splitlines()
        event_name = next(line.removeprefix("event: ") for line in lines if line.startswith("event: "))
        data = json.loads(next(line.removeprefix("data: ") for line in lines if line.startswith("data: ")))
        events.append({"event": event_name, "data": data})
    return events


def assert_uuid_string(value: object) -> None:
    assert isinstance(value, str)
    assert len(value) == 36
    assert value.count("-") == 4


@pytest.mark.parametrize("path", ["/", "/health"])
def test_smoke_endpoints_require_token(path: str) -> None:
    response = client.get(path)

    assert response.status_code == 401
    assert response.json() == {"detail": "Missing or invalid bearer token"}


def test_root_returns_dummy_message_with_token() -> None:
    response = client.get("/", headers={"Authorization": "Bearer test-token"})

    assert response.status_code == 200
    assert response.json() == {"message": "Hallo!"}


def test_health_returns_ok_with_token() -> None:
    response = client.get("/health", headers={"Authorization": "Bearer test-token"})

    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


@pytest.mark.parametrize("path", ["/voice", "/voice/debug", "/voice/stream"])
def test_legacy_voice_endpoints_are_removed(path: str) -> None:
    response = client.post(
        path,
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("sample.wav", make_dummy_wav(), "audio/wav")},
    )

    assert response.status_code == 404


def test_chat_stream_text_sends_message_events_with_tts_by_default(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_stream_agent_bridge(prompt: str):
        assert prompt == "hallo user"
        yield {"type": "commentary", "text": "Ich suche kurz."}
        yield {"type": "final", "text": "Hallo User."}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return f"mp3:{text}".encode()

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "  hallo user  "},
    )

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/event-stream")
    events = parse_sse_events(response.text)
    assert [event["event"] for event in events] == ["message", "message", "message", "done"]

    user = events[0]["data"]
    commentary = events[1]["data"]
    final = events[2]["data"]

    assert user["role"] == "user"
    assert user["phase"] == "input"
    assert user["sequence"] == 0
    assert user["text"] == "hallo user"
    assert_uuid_string(user["message_id"])
    assert_uuid_string(user["turn_id"])

    assert commentary["role"] == "assistant"
    assert commentary["phase"] == "commentary"
    assert commentary["sequence"] == 1
    assert commentary["turn_id"] == user["turn_id"]
    assert commentary["text"] == "Ich suche kurz."
    assert commentary["audio_format"] == "mp3"
    assert commentary["audio_base64"] == base64.b64encode(b"mp3:Ich suche kurz.").decode("ascii")

    assert final["role"] == "assistant"
    assert final["phase"] == "final"
    assert final["sequence"] == 2
    assert final["turn_id"] == user["turn_id"]
    assert final["text"] == "Hallo User."
    assert final["audio_format"] == "mp3"
    assert final["audio_base64"] == base64.b64encode(b"mp3:Hallo User.").decode("ascii")
    assert events[3] == {"event": "done", "data": {"type": "done"}}


def test_chat_stream_strips_leading_wake_word_from_text(monkeypatch: pytest.MonkeyPatch) -> None:
    prompts = []

    async def fake_stream_agent_bridge(prompt: str):
        prompts.append(prompt)
        yield {"type": "final", "text": "ok"}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return b"mp3"

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "Alexa, wie spät ist es?"},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    assert prompts == ["wie spät ist es?"]
    assert events[0]["data"]["text"] == "wie spät ist es?"


def test_chat_stream_keeps_wake_word_inside_text(monkeypatch: pytest.MonkeyPatch) -> None:
    prompts = []

    async def fake_stream_agent_bridge(prompt: str):
        prompts.append(prompt)
        yield {"type": "final", "text": "ok"}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return b"mp3"

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "Warum sagt Alexa das?"},
    )

    assert response.status_code == 200
    assert prompts == ["Warum sagt Alexa das?"]


def test_chat_stream_keeps_only_wake_word(monkeypatch: pytest.MonkeyPatch) -> None:
    prompts = []

    async def fake_stream_agent_bridge(prompt: str):
        prompts.append(prompt)
        yield {"type": "final", "text": "ok"}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return b"mp3"

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "Alexa"},
    )

    assert response.status_code == 200
    assert prompts == ["Alexa"]


def test_chat_stream_can_request_wav_tts(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_stream_agent_bridge(prompt: str):
        assert prompt == "hallo user"
        yield {"type": "final", "text": "Hallo User."}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return f"mp3:{text}".encode()

    def fake_convert_mp3_to_wav(mp3_bytes: bytes) -> bytes:
        assert mp3_bytes == b"mp3:Hallo User."
        return b"wav:Hallo User."

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)
    monkeypatch.setattr("siri_backend.app.convert_mp3_to_wav", fake_convert_mp3_to_wav)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "hallo user", "tts_format": "wav"},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    final = events[1]["data"]
    assert final["phase"] == "final"
    assert final["audio_format"] == "wav"
    assert final["audio_base64"] == base64.b64encode(b"wav:Hallo User.").decode("ascii")


def test_chat_stream_rejects_invalid_tts_format(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fail_stream_agent_bridge(prompt: str):
        raise AssertionError("Agent bridge should not be called for invalid tts_format")
        yield

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fail_stream_agent_bridge)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "hallo user", "tts_format": "flac"},
    )

    assert response.status_code == 400
    assert response.json() == {
        "detail": {"error_code": "invalid_tts_format", "message": "tts_format must be mp3 or wav"}
    }


def test_chat_stream_audio_strips_leading_wake_word(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_transcribe_audio(*, audio_bytes: bytes, filename: str, content_type: str | None) -> str:
        return "  Alexa - wie spät ist es?  "

    async def fake_stream_agent_bridge(prompt: str):
        assert prompt == "wie spät ist es?"
        yield {"type": "final", "text": "Antwort aus Audio."}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return f"mp3:{text}".encode()

    monkeypatch.setattr("siri_backend.app.transcribe_audio", fake_transcribe_audio)
    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("prompt.webm", b"fake webm bytes", "audio/webm;codecs=opus")},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    assert events[0]["data"]["text"] == "wie spät ist es?"


def test_chat_stream_audio_sends_transcript_as_user_message(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_transcribe_audio(*, audio_bytes: bytes, filename: str, content_type: str | None) -> str:
        assert audio_bytes == b"fake webm bytes"
        assert filename == "prompt.webm"
        assert content_type == "audio/webm;codecs=opus"
        return "  hallo aus audio  "

    async def fake_stream_agent_bridge(prompt: str):
        assert prompt == "hallo aus audio"
        yield {"type": "commentary", "text": "Ich habe dich verstanden."}
        yield {"type": "final", "text": "Antwort aus Audio."}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return f"mp3:{text}".encode()

    monkeypatch.setattr("siri_backend.app.transcribe_audio", fake_transcribe_audio)
    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("prompt.webm", b"fake webm bytes", "audio/webm;codecs=opus")},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    assert [event["event"] for event in events] == ["message", "message", "message", "done"]
    assert events[0]["data"]["text"] == "hallo aus audio"
    assert events[1]["data"]["phase"] == "commentary"
    assert events[2]["data"]["phase"] == "final"


def test_chat_stream_can_disable_tts(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_stream_agent_bridge(prompt: str):
        yield {"type": "commentary", "text": "Ich suche kurz."}
        yield {"type": "final", "text": "Hallo User."}
        yield {"type": "done"}

    async def fail_synthesize_speech(text: str) -> bytes:
        raise AssertionError("TTS should not run when tts_enabled=false")

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fail_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "hallo user", "tts_enabled": "false", "tts_format": "wav"},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    assert [event["event"] for event in events] == ["message", "message", "message", "done"]
    assert all("audio_base64" not in event["data"] for event in events)
    assert all("audio_format" not in event["data"] for event in events)


def test_chat_stream_text_validates_input(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fail_stream_agent_bridge(prompt: str):
        raise AssertionError("Agent bridge should not be called for invalid chat input")
        yield

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fail_stream_agent_bridge)

    missing = client.post("/chat/stream", headers={"Authorization": "Bearer test-token"})
    blank = client.post("/chat/stream", headers={"Authorization": "Bearer test-token"}, data={"text": "   "})
    with_audio = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "hallo"},
        files={"audio": ("sample.wav", make_dummy_wav(), "audio/wav")},
    )

    assert missing.status_code == 400
    assert missing.json() == {"detail": {"error_code": "invalid_chat_input", "message": "Text input is required"}}
    assert blank.status_code == 400
    assert blank.json() == {"detail": {"error_code": "invalid_chat_input", "message": "Text input is required"}}
    assert with_audio.status_code == 400
    assert with_audio.json() == {
        "detail": {"error_code": "invalid_chat_input", "message": "Provide exactly one of text or audio"}
    }


def test_chat_stream_audio_validation_errors_use_stable_chat_shape(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fail_transcribe_audio(*, audio_bytes: bytes, filename: str, content_type: str | None) -> str:
        raise AssertionError("STT should not be called for invalid uploads")

    monkeypatch.setattr("siri_backend.app.transcribe_audio", fail_transcribe_audio)

    empty = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("empty.wav", b"", "audio/wav")},
    )
    unsupported = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("sample.txt", b"not audio", "text/plain")},
    )
    oversized = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("huge.wav", b"0" * (MAX_UPLOAD_BYTES + 1), "audio/wav")},
    )

    assert empty.status_code == 400
    assert empty.json() == {"detail": {"error_code": "invalid_chat_input", "message": "Audio upload is empty"}}
    assert unsupported.status_code == 400
    assert unsupported.json() == {
        "detail": {"error_code": "invalid_chat_input", "message": "Unsupported audio type: text/plain"}
    }
    assert oversized.status_code == 413
    assert oversized.json() == {
        "detail": {"error_code": "invalid_chat_input", "message": f"Audio upload exceeds {MAX_UPLOAD_BYTES} bytes"}
    }


def test_chat_stream_audio_returns_stable_error_when_stt_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_transcribe_audio(*, audio_bytes: bytes, filename: str, content_type: str | None) -> str:
        raise RuntimeError("secret provider failure")

    async def fail_stream_agent_bridge(prompt: str):
        raise AssertionError("Agent bridge should not be called after STT failure")
        yield

    monkeypatch.setattr("siri_backend.app.transcribe_audio", fake_transcribe_audio)
    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fail_stream_agent_bridge)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        files={"audio": ("sample.wav", make_dummy_wav(), "audio/wav")},
    )

    assert response.status_code == 502
    assert response.json() == {"detail": {"error_code": "stt_failed", "message": "Speech-to-text failed"}}
    assert "secret" not in response.text


def test_chat_stream_interruption_ends_old_stream_cleanly(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_stream_agent_bridge(prompt: str):
        yield {"type": "commentary", "text": "Ich arbeite noch."}
        yield {"type": "interrupted"}
        yield {"type": "final", "text": "Diese alte Antwort darf nicht mehr erscheinen."}
        yield {"type": "done"}

    async def fake_synthesize_speech(text: str) -> bytes:
        return f"mp3:{text}".encode()

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fake_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "first slow prompt"},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    assert [event["event"] for event in events] == ["message", "message", "interrupted", "done"]
    assert "Diese alte Antwort" not in response.text


def test_chat_stream_sends_stable_error_codes(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_stream_agent_bridge(prompt: str):
        yield {"type": "error", "detail": "secret provider internals"}
        yield {"type": "done"}

    async def fail_synthesize_speech(text: str) -> bytes:
        raise AssertionError("TTS should not run for error events")

    monkeypatch.setattr("siri_backend.app.stream_agent_bridge", fake_stream_agent_bridge)
    monkeypatch.setattr("siri_backend.app.synthesize_speech", fail_synthesize_speech)

    response = client.post(
        "/chat/stream",
        headers={"Authorization": "Bearer test-token"},
        data={"text": "hallo user"},
    )

    assert response.status_code == 200
    events = parse_sse_events(response.text)
    assert [event["event"] for event in events] == ["message", "error", "done"]
    assert events[1] == {
        "event": "error",
        "data": {"type": "error", "error_code": "agent_failed", "message": "Agent failed"},
    }
    assert "secret" not in response.text


def test_stream_agent_bridge_sends_bearer_auth_and_parses_jsonl(monkeypatch: pytest.MonkeyPatch) -> None:
    captured = {}

    class FakeStreamResponse:
        status_code = 200

        async def __aenter__(self):
            return self

        async def __aexit__(self, exc_type, exc, tb):
            return None

        async def aiter_lines(self):
            yield '{"type":"commentary","text":"Ich suche."}'
            yield ""
            yield '{"type":"final","text":"Fertig."}'
            yield '{"type":"done"}'

    class FakeAsyncClient:
        def __init__(self, *, timeout: float):
            captured["timeout"] = timeout

        async def __aenter__(self):
            return self

        async def __aexit__(self, exc_type, exc, tb):
            return None

        def stream(self, method: str, url: str, *, headers: dict[str, str], json: dict[str, str]):
            captured["method"] = method
            captured["url"] = url
            captured["headers"] = headers
            captured["json"] = json
            return FakeStreamResponse()

    monkeypatch.setattr("siri_backend.app.AGENT_BRIDGE_URL", "http://agent-bridge:8010/ask")
    monkeypatch.setattr("siri_backend.app.AGENT_BRIDGE_TOKEN", "test-agent-token")
    monkeypatch.setattr("siri_backend.app.AGENT_TIMEOUT_SECONDS", 60.0)
    monkeypatch.setattr("httpx.AsyncClient", FakeAsyncClient)

    from siri_backend.app import stream_agent_bridge

    events = []

    async def collect() -> None:
        async for event in stream_agent_bridge("hallo user"):
            events.append(event)

    asyncio.run(collect())

    assert captured == {
        "timeout": 60.0,
        "method": "POST",
        "url": "http://agent-bridge:8010/ask/stream",
        "headers": {"Authorization": "Bearer test-agent-token"},
        "json": {"prompt": "hallo user", "session_mode": "persistent"},
    }
    assert events == [
        {"type": "commentary", "text": "Ich suche."},
        {"type": "final", "text": "Fertig."},
        {"type": "done"},
    ]


def test_synthesize_speech_passes_integer_edge_tts_timeouts(monkeypatch: pytest.MonkeyPatch) -> None:
    captured = {}

    class FakeCommunicate:
        def __init__(self, text, voice, rate, connect_timeout, receive_timeout):
            captured["text"] = text
            captured["voice"] = voice
            captured["rate"] = rate
            captured["connect_timeout"] = connect_timeout
            captured["receive_timeout"] = receive_timeout

        async def save(self, path):
            Path(path).write_bytes(b"ID3 fake edge tts")

    fake_edge_tts = SimpleNamespace(Communicate=FakeCommunicate)
    monkeypatch.setitem(sys.modules, "edge_tts", fake_edge_tts)
    monkeypatch.setattr("siri_backend.app.TTS_TIMEOUT_SECONDS", 30.5)
    monkeypatch.setattr("siri_backend.app.EDGE_TTS_RATE", "+25%")

    from siri_backend.app import synthesize_speech

    mp3_bytes = asyncio.run(synthesize_speech("Hallo"))

    assert mp3_bytes == b"ID3 fake edge tts"
    assert captured["rate"] == "+25%"
    assert captured["connect_timeout"] == 31
    assert captured["receive_timeout"] == 31
    assert isinstance(captured["connect_timeout"], int)
    assert isinstance(captured["receive_timeout"], int)


def test_voice_session_reset_requires_bearer_token() -> None:
    response = client.post("/voice/session/reset")

    assert response.status_code == 401


def test_voice_session_reset_calls_bridge_reset(monkeypatch: pytest.MonkeyPatch) -> None:
    captured = {}

    class FakeResponse:
        status_code = 200

    class FakeAsyncClient:
        def __init__(self, *, timeout: float):
            captured["timeout"] = timeout

        async def __aenter__(self):
            return self

        async def __aexit__(self, exc_type, exc, tb):
            return None

        async def post(self, url: str, *, headers: dict[str, str]):
            captured["url"] = url
            captured["headers"] = headers
            return FakeResponse()

    monkeypatch.setattr("siri_backend.app.AGENT_BRIDGE_URL", "http://agent-bridge:8010/ask")
    monkeypatch.setattr("siri_backend.app.AGENT_BRIDGE_TOKEN", "test-agent-token")
    monkeypatch.setattr("siri_backend.app.AGENT_TIMEOUT_SECONDS", 60.0)
    monkeypatch.setattr("httpx.AsyncClient", FakeAsyncClient)

    response = client.post("/voice/session/reset", headers={"Authorization": "Bearer test-token"})

    assert response.status_code == 200
    assert response.json() == {"status": "reset"}
    assert captured == {
        "timeout": 60.0,
        "url": "http://agent-bridge:8010/ask/session/reset",
        "headers": {"Authorization": "Bearer test-agent-token"},
    }
