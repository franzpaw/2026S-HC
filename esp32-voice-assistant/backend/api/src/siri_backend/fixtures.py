from io import BytesIO
import wave


FAKE_MP3_BYTES = b"ID3\x04\x00\x00\x00\x00\x00\x00FAKE_MP3_RESPONSE"


def make_dummy_wav() -> bytes:
    buffer = BytesIO()
    with wave.open(buffer, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(16_000)
        wav_file.writeframes(b"\x00\x00" * 16_000)
    return buffer.getvalue()
