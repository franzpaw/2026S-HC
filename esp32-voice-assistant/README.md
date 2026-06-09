# ESP32-S3 Voice Assistant

Hybrid Edge/Cloud voice assistant for a Waveshare ESP32-S3 Audio Board.

The ESP32 handles hardware-near work:

```text
wakeword, recording, LED feedback, Wi-Fi upload, speaker playback
```

The backend handles compute-heavy work:

```text
STT, agent orchestration, web search, skills, TTS
```

## Architecture

```text
ESP32 WakeNet wakeword
-> record user speech as WAV
-> upload to backend
-> Groq STT
-> Pi/Codex agent bridge
-> optional web_search / skills
-> Edge TTS
-> WAV response
-> ESP32 speaker playback
-> wait for next wakeword
```

## Folder Structure

```text
esp32-voice-assistant/
  firmware/
    esp32-s3/       ESP-IDF firmware for Waveshare ESP32-S3 Audio Board
  backend/
    api/            FastAPI voice API: auth, STT, agent call, TTS, SSE stream
    agent-bridge/   Node/TypeScript bridge to Pi/Codex
    deploy/         Docker Compose runtime and Pi profile
    rust-cli/       Rust test/demo client
```

More detail:

```text
firmware/esp32-s3/README.md
backend/README.md
```

## Local Development

Backend:

```bash
cd esp32-voice-assistant/backend/deploy
cp .env.example .env
# edit .env
docker compose up --build
```

Firmware:

```bash
cd esp32-voice-assistant/firmware/esp32-s3
source /path/to/esp-idf/export.sh
idf.py menuconfig
idf.py build
```

Set in menuconfig:

```text
Voice Client -> Wi-Fi SSID
Voice Client -> Wi-Fi password
Voice Client -> Backend base URL
Voice Client -> Voice API bearer token
```

## Testing

Backend health via Rust CLI:

```bash
cd esp32-voice-assistant/backend/rust-cli
cargo run -p siri-cli -- --backend-url http://127.0.0.1:8000 --token test-token health
```

Text request:

```bash
cargo run -p siri-cli -- --backend-url http://127.0.0.1:8000 --token test-token --no-tts chat text "Hallo"
```

Hardware smoke:

```text
1. Start backend.
2. Flash ESP32 firmware.
3. Say "Alexa".
4. Speak a command.
5. Wait for spoken response.
```

## Deployment

Backend needs a reachable Docker host:

```text
VPS, home server, or local machine on same network
```

For ESP32 outside the same machine/network, expose backend through HTTPS domain/reverse proxy:

```text
ESP32 -> https://<backend-domain> -> reverse proxy -> Docker backend
```

The ESP32 backend URL and token are configured through ESP-IDF menuconfig and stored in local `sdkconfig`.

## Local Secrets

Do not commit:

```text
backend/deploy/.env
backend/deploy/.pi/agent/auth.json
backend/deploy/.pi/agent/sessions/
backend/deploy/.pi/logs/
firmware/esp32-s3/sdkconfig
```
