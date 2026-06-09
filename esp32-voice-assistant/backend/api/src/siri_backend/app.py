import asyncio
import base64
from collections.abc import AsyncIterator
from dataclasses import dataclass
import json
import logging
import math
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time
from uuid import uuid4

import httpx
from fastapi import Depends, FastAPI, File, Form, Header, HTTPException, Request, UploadFile, status
from fastapi.responses import StreamingResponse

EXPECTED_TOKEN = os.getenv("VOICE_API_TOKEN", "test-token")
GROQ_STT_MODEL = os.getenv("GROQ_STT_MODEL", "whisper-large-v3-turbo")
EDGE_TTS_VOICE = os.getenv("EDGE_TTS_VOICE", "de-DE-KatjaNeural")
EDGE_TTS_RATE = os.getenv("EDGE_TTS_RATE", "+25%")
STT_TIMEOUT_SECONDS = float(os.getenv("STT_TIMEOUT_SECONDS", "30"))
TTS_TIMEOUT_SECONDS = float(os.getenv("TTS_TIMEOUT_SECONDS", "30"))
AGENT_TIMEOUT_SECONDS = float(os.getenv("AGENT_TIMEOUT_SECONDS", "60"))
AGENT_BRIDGE_URL = os.getenv("AGENT_BRIDGE_URL", "http://agent-bridge:8010/ask")
AGENT_BRIDGE_TOKEN = os.getenv("AGENT_BRIDGE_TOKEN", "test-agent-token")
MAX_UPLOAD_BYTES = int(os.getenv("MAX_UPLOAD_BYTES", str(5 * 1024 * 1024)))
SUPPORTED_AUDIO_TYPES = {
    "audio/flac",
    "audio/m4a",
    "audio/mp4",
    "audio/mp3",
    "audio/mpeg",
    "audio/mpga",
    "audio/ogg",
    "audio/wav",
    "audio/wave",
    "audio/webm",
    "audio/x-m4a",
    "video/mp4",
    "video/webm",
    "audio/x-wav",
}
SPOKEN_STREAM_TYPES = {"commentary", "final"}
PASSTHROUGH_STREAM_TYPES = {"error", "done", "interrupted"}
SUPPORTED_TTS_FORMATS = {"mp3", "wav"}

logger = logging.getLogger(__name__)
app = FastAPI(title="Siri Voice Backend")


class AgentTimeoutError(RuntimeError):
    pass


class AgentBridgeError(RuntimeError):
    pass


@dataclass(frozen=True)
class AudioUpload:
    audio_bytes: bytes
    filename: str
    content_type: str | None


@app.middleware("http")
async def log_voice_request(request: Request, call_next):
    if request.url.path in {"/chat/stream", "/voice/session/reset"}:
        logger.info("Voice request started")
    response = await call_next(request)
    if request.url.path in {"/chat/stream", "/voice/session/reset"}:
        logger.info("Voice response status=%d", response.status_code)
    return response


def require_voice_token(authorization: str | None = Header(default=None)) -> None:
    expected = f"Bearer {EXPECTED_TOKEN}"
    if authorization != expected:
        logger.info("Voice auth rejected")
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Missing or invalid bearer token",
        )
    logger.info("Voice auth accepted")


@app.get("/", dependencies=[Depends(require_voice_token)])
def root() -> dict[str, str]:
    return {"message": "Hallo!"}


@app.get("/health", dependencies=[Depends(require_voice_token)])
def health() -> dict[str, str]:
    return {"status": "ok"}


async def transcribe_audio(*, audio_bytes: bytes, filename: str, content_type: str | None) -> str:
    from groq import AsyncGroq

    api_key = os.getenv("GROQ_API_KEY")
    if not api_key:
        raise RuntimeError("GROQ_API_KEY is not configured")

    client = AsyncGroq(api_key=api_key, timeout=STT_TIMEOUT_SECONDS)
    transcription = await client.audio.transcriptions.create(
        model=GROQ_STT_MODEL,
        file=(filename, audio_bytes, content_type or "application/octet-stream"),
    )
    text = getattr(transcription, "text", "")
    if not text:
        raise RuntimeError("Groq transcription did not contain text")
    return text


async def synthesize_speech(text: str) -> bytes:
    import edge_tts

    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as output_file:
        output_path = Path(output_file.name)

    try:
        communicate = edge_tts.Communicate(
            text,
            EDGE_TTS_VOICE,
            rate=EDGE_TTS_RATE,
            connect_timeout=edge_tts_timeout_seconds(),
            receive_timeout=edge_tts_timeout_seconds(),
        )
        await communicate.save(str(output_path))
        return output_path.read_bytes()
    finally:
        output_path.unlink(missing_ok=True)


def edge_tts_timeout_seconds() -> int:
    return max(1, math.ceil(TTS_TIMEOUT_SECONDS))


def validate_tts_format(tts_format: str) -> str:
    normalized = tts_format.strip().lower()
    if normalized not in SUPPORTED_TTS_FORMATS:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail={"error_code": "invalid_tts_format", "message": "tts_format must be mp3 or wav"},
        )
    return normalized


def convert_mp3_to_wav(mp3_bytes: bytes) -> bytes:
    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as input_file:
        input_path = Path(input_file.name)
        input_file.write(mp3_bytes)
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as output_file:
        output_path = Path(output_file.name)

    try:
        subprocess.run(
            ["ffmpeg", "-nostdin", "-y", "-i", str(input_path), "-ac", "1", "-ar", "16000", str(output_path)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return output_path.read_bytes()
    finally:
        input_path.unlink(missing_ok=True)
        output_path.unlink(missing_ok=True)


async def stream_agent_bridge(transcript: str) -> AsyncIterator[dict[str, object]]:
    try:
        async with httpx.AsyncClient(timeout=AGENT_TIMEOUT_SECONDS) as client:
            async with client.stream(
                "POST",
                agent_bridge_stream_url(),
                headers={"Authorization": f"Bearer {AGENT_BRIDGE_TOKEN}"},
                json={"prompt": transcript, "session_mode": "persistent"},
            ) as response:
                if response.status_code == status.HTTP_504_GATEWAY_TIMEOUT:
                    raise AgentTimeoutError("Agent timed out")
                if response.status_code >= 400:
                    raise AgentBridgeError("Agent bridge returned an error")

                async for line in response.aiter_lines():
                    if not line.strip():
                        continue
                    try:
                        event = json.loads(line)
                    except ValueError as error:
                        raise AgentBridgeError("Agent bridge returned invalid JSONL") from error
                    if not isinstance(event, dict) or not isinstance(event.get("type"), str):
                        raise AgentBridgeError("Agent bridge returned invalid stream event")
                    yield event
    except httpx.TimeoutException as error:
        raise AgentTimeoutError("Agent timed out") from error
    except httpx.HTTPError as error:
        raise AgentBridgeError("Agent bridge request failed") from error


def agent_bridge_stream_url() -> str:
    base = AGENT_BRIDGE_URL.rstrip("/")
    return f"{base}/stream" if base.endswith("/ask") else f"{base}/ask/stream"


def agent_bridge_reset_url() -> str:
    base = AGENT_BRIDGE_URL.rstrip("/")
    if base.endswith("/ask"):
        return f"{base}/session/reset"
    if base.endswith("/ask/stream"):
        return f"{base.removesuffix('/stream')}/session/reset"
    return f"{base}/ask/session/reset"


async def reset_agent_bridge() -> None:
    try:
        async with httpx.AsyncClient(timeout=AGENT_TIMEOUT_SECONDS) as client:
            response = await client.post(
                agent_bridge_reset_url(),
                headers={"Authorization": f"Bearer {AGENT_BRIDGE_TOKEN}"},
            )
    except httpx.TimeoutException as error:
        raise AgentTimeoutError("Agent reset timed out") from error
    except httpx.HTTPError as error:
        raise AgentBridgeError("Agent reset request failed") from error

    if response.status_code == status.HTTP_504_GATEWAY_TIMEOUT:
        raise AgentTimeoutError("Agent reset timed out")
    if response.status_code >= 400:
        raise AgentBridgeError("Agent reset failed")


async def read_audio_upload(audio: UploadFile | None) -> AudioUpload:
    if audio is None:
        logger.info("Audio upload rejected: missing")
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Audio upload is required")

    audio_bytes = await audio.read(MAX_UPLOAD_BYTES + 1)
    content_type = audio.content_type

    if not audio_bytes:
        logger.info("Audio upload rejected: empty")
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Audio upload is empty")
    content_type_base = content_type.split(";", 1)[0].strip().lower() if content_type else None
    if content_type_base not in SUPPORTED_AUDIO_TYPES:
        logger.info("Audio upload rejected: unsupported content type %s", content_type or "missing")
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Unsupported audio type: {content_type or 'missing content-type'}",
        )
    if len(audio_bytes) > MAX_UPLOAD_BYTES:
        logger.info("Audio upload rejected: %d bytes exceeds limit", len(audio_bytes))
        raise HTTPException(
            status_code=status.HTTP_413_CONTENT_TOO_LARGE,
            detail=f"Audio upload exceeds {MAX_UPLOAD_BYTES} bytes",
        )

    filename = audio.filename or "audio.wav"
    logger.info("Audio upload accepted: filename=%s content_type=%s bytes=%d", filename, content_type, len(audio_bytes))
    return AudioUpload(audio_bytes=audio_bytes, filename=filename, content_type=content_type)


async def recognize_transcript(upload: AudioUpload) -> tuple[str, float]:
    try:
        stt_started_at = time.perf_counter()
        async with asyncio.timeout(STT_TIMEOUT_SECONDS):
            transcript = await transcribe_audio(
                audio_bytes=upload.audio_bytes,
                filename=upload.filename,
                content_type=upload.content_type,
            )
    except TimeoutError:
        logger.error("Speech-to-text timed out")
        raise HTTPException(status_code=status.HTTP_504_GATEWAY_TIMEOUT, detail="Speech-to-text timed out") from None
    except Exception:
        logger.error("Speech-to-text failed")
        raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail="Speech-to-text failed") from None

    stt_ms = elapsed_ms(stt_started_at)
    normalized_transcript = transcript.strip()
    logger.info("Transcript recognized: chars=%d", len(normalized_transcript))
    if len(normalized_transcript) < 3:
        logger.info("Transcript rejected: too short")
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Transcript is too short")
    return normalized_transcript, stt_ms


async def synthesize_timed(text: str) -> tuple[bytes, float]:
    try:
        tts_started_at = time.perf_counter()
        async with asyncio.timeout(TTS_TIMEOUT_SECONDS):
            mp3_bytes = await synthesize_speech(text)
    except TimeoutError:
        logger.error("Text-to-speech timed out")
        raise HTTPException(status_code=status.HTTP_504_GATEWAY_TIMEOUT, detail="Text-to-speech timed out") from None
    except HTTPException:
        raise
    except Exception:
        logger.error("Text-to-speech failed")
        raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail="Text-to-speech failed") from None
    return mp3_bytes, elapsed_ms(tts_started_at)


def chat_input_error(message: str) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_400_BAD_REQUEST,
        detail={"error_code": "invalid_chat_input", "message": message},
    )


def chat_http_error_from_voice_error(error: HTTPException) -> HTTPException:
    detail = str(error.detail)
    error_map = {
        "Speech-to-text timed out": "stt_timeout",
        "Speech-to-text failed": "stt_failed",
        "Transcript is too short": "stt_invalid_transcript",
    }
    return HTTPException(
        status_code=error.status_code,
        detail={"error_code": error_map.get(detail, "invalid_chat_input"), "message": detail},
    )


def strip_leading_wake_word(text: str) -> str:
    stripped = re.sub(r"^alexa[\s,;:.!?-]+", "", text.strip(), count=1, flags=re.IGNORECASE).strip()
    return stripped or text.strip()


async def resolve_chat_input(text: str | None, audio: UploadFile | None) -> str:
    if text is not None and audio is not None:
        raise chat_input_error("Provide exactly one of text or audio")
    if text is not None:
        normalized_text = text.strip()
        if not normalized_text:
            raise chat_input_error("Text input is required")
        return strip_leading_wake_word(normalized_text)
    if audio is None:
        raise chat_input_error("Text input is required")

    try:
        upload = await read_audio_upload(audio)
        transcript, _stt_ms = await recognize_transcript(upload)
    except HTTPException as error:
        raise chat_http_error_from_voice_error(error) from None
    return strip_leading_wake_word(transcript)


async def stream_chat_text_events(text: str, *, tts_enabled: bool, tts_format: str) -> AsyncIterator[str]:
    sequence = 0
    turn_id = str(uuid4())
    yield sse_event(
        "message",
        {
            "type": "message",
            "role": "user",
            "phase": "input",
            "sequence": sequence,
            "message_id": str(uuid4()),
            "turn_id": turn_id,
            "text": text,
        },
    )

    try:
        async for bridge_event in stream_agent_bridge(text):
            event_type = bridge_event.get("type")
            if event_type in SPOKEN_STREAM_TYPES:
                phase = "commentary" if event_type == "commentary" else "final"
                event_text = bridge_event.get("text")
                if not isinstance(event_text, str) or not event_text.strip():
                    continue
                sequence += 1
                message: dict[str, object] = {
                    "type": "message",
                    "role": "assistant",
                    "phase": phase,
                    "sequence": sequence,
                    "message_id": str(uuid4()),
                    "turn_id": turn_id,
                    "text": event_text,
                }
                if tts_enabled:
                    try:
                        audio_bytes, _tts_ms = await synthesize_timed(event_text)
                    except HTTPException as error:
                        yield sse_event("error", chat_error_from_detail(error.detail))
                        yield sse_event("done", {"type": "done"})
                        return
                    audio_format = "mp3"
                    if tts_format == "wav":
                        audio_bytes = convert_mp3_to_wav(audio_bytes)
                        audio_format = "wav"
                    message["audio_format"] = audio_format
                    message["audio_base64"] = base64.b64encode(audio_bytes).decode("ascii")
                yield sse_event("message", message)
                continue

            if event_type == "error":
                detail = bridge_event.get("detail")
                if isinstance(detail, str) and detail in {"Agent timed out", "Agent failed"}:
                    safe_detail = detail
                else:
                    safe_detail = "Agent failed"
                yield sse_event("error", chat_error_from_detail(safe_detail))
                continue

            if event_type == "interrupted":
                yield sse_event("interrupted", {"type": "interrupted"})
                yield sse_event("done", {"type": "done"})
                return

            if event_type in PASSTHROUGH_STREAM_TYPES:
                yield sse_event(event_type, {"type": event_type})
    except (TimeoutError, AgentTimeoutError):
        logger.error("Agent timed out")
        yield sse_event("error", chat_error_from_detail("Agent timed out"))
        yield sse_event("done", {"type": "done"})
    except Exception:
        logger.error("Agent failed")
        yield sse_event("error", chat_error_from_detail("Agent failed"))
        yield sse_event("done", {"type": "done"})


def chat_error_from_detail(detail: object) -> dict[str, object]:
    error_map = {
        "Agent timed out": ("agent_timeout", "Agent timed out"),
        "Text-to-speech timed out": ("tts_timeout", "Text-to-speech timed out"),
        "Text-to-speech failed": ("tts_failed", "Text-to-speech failed"),
    }
    error_code, message = error_map.get(str(detail), ("agent_failed", "Agent failed"))
    return {"type": "error", "error_code": error_code, "message": message}


def sse_event(event: object, data: dict[str, object]) -> str:
    return f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"


def elapsed_ms(started_at: float) -> float:
    return round((time.perf_counter() - started_at) * 1000, 3)


@app.post("/chat/stream")
async def chat_stream(
    text: str | None = Form(default=None),
    tts_enabled: bool = Form(default=True),
    tts_format: str = Form(default="mp3"),
    audio: UploadFile | None = File(default=None),
    _: None = Depends(require_voice_token),
) -> StreamingResponse:
    normalized_text = await resolve_chat_input(text, audio)
    normalized_tts_format = validate_tts_format(tts_format)
    return StreamingResponse(
        stream_chat_text_events(normalized_text, tts_enabled=tts_enabled, tts_format=normalized_tts_format),
        media_type="text/event-stream",
    )


@app.post("/voice/session/reset")
async def voice_session_reset(_: None = Depends(require_voice_token)) -> dict[str, str]:
    try:
        await reset_agent_bridge()
    except (TimeoutError, AgentTimeoutError):
        logger.error("Agent reset timed out")
        raise HTTPException(status_code=status.HTTP_504_GATEWAY_TIMEOUT, detail="Agent reset timed out") from None
    except Exception:
        logger.error("Agent reset failed")
        raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail="Agent reset failed") from None
    return {"status": "reset"}
