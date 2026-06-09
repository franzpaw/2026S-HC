# Backend

Voice backend for the ESP32-S3 assistant.

## Structure

```text
backend/
  api/                 FastAPI service: auth, audio upload, STT, agent call, TTS, SSE stream
  agent-bridge/        Node/TypeScript bridge: runs Pi agent session and streams text events
  deploy/              Docker Compose runtime and env template
    .pi/agent/         Pi profile mounted into agent-bridge container
      SYSTEM.md        Voice-agent system prompt
      settings.json    Pi extension config
      extensions/      Pi extensions, e.g. codex web_search
      skills/          Pi skills, e.g. dummy skill
  rust-cli/            Rust test client for health/reset/text/audio calls
```

## Runtime Flow

```text
ESP32 -> backend/api -> agent-bridge -> Pi agent -> backend/api -> ESP32
```

Backend does:

```text
Groq STT
Pi/Codex agent orchestration
Edge TTS
WAV audio response
```

ESP32 keeps hardware work:

```text
wakeword, recording, LEDs, speaker playback
```

## Config

Copy env template:

```bash
cd backend/deploy
cp .env.example .env
```

Set at least:

```text
VOICE_API_TOKEN       bearer token for ESP/CLI -> backend
AGENT_BRIDGE_TOKEN    internal backend -> agent-bridge token
GROQ_API_KEY          STT provider key
EDGE_TTS_VOICE        spoken voice
EDGE_TTS_RATE         speech speed, e.g. +25%
```

Do not commit:

```text
deploy/.env
deploy/.pi/agent/auth.json
deploy/.pi/agent/sessions/
deploy/.pi/logs/
```

## Local Run

```bash
cd backend/deploy
docker compose up --build
```

Backend listens locally:

```text
http://127.0.0.1:8000
```

## Testing

Rust CLI is only a test/demo client.

```bash
cd backend/rust-cli
cargo run -p siri-cli -- --backend-url http://127.0.0.1:8000 --token test-token health
cargo run -p siri-cli -- --backend-url http://127.0.0.1:8000 --token test-token --no-tts chat text "Hallo"
```

Audio test:

```bash
cargo run -p siri-cli -- --backend-url http://127.0.0.1:8000 --token test-token chat voice --file sample.wav
```

## Skills And Extensions

Pi runtime lives here:

```text
backend/deploy/.pi/agent/
```

Add a skill:

```text
deploy/.pi/agent/skills/<skill-name>/SKILL.md
```

Add an extension:

```text
deploy/.pi/agent/extensions/<extension-name>/package.json
deploy/.pi/agent/extensions/<extension-name>/index.ts
```

Enable extension in:

```text
deploy/.pi/agent/settings.json
```

After changing prompt, skill, or extension:

```bash
cd backend/deploy
docker compose restart agent-bridge
```

If backend Python/Node code changed:

```bash
docker compose up -d --build
```

## Deployment

Need one reachable host:

```text
VPS, home server, or any Docker host reachable by ESP32
```

If ESP32 is not on same machine/network, expose backend through HTTPS domain/reverse proxy.

Typical production:

```text
ESP32 -> https://<your-domain> -> reverse proxy -> 127.0.0.1:8000 -> Docker backend
```

Set ESP32 backend URL in ESP-IDF menuconfig:

```text
Voice Client -> Backend base URL
```

Secrets stay outside Git:

```text
backend/deploy/.env
ESP32 sdkconfig
```
