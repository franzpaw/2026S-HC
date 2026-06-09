import { createBridgeServer } from "./app.js";
import { createPiSdkStreamController } from "./stream-runner.js";
import { syncVoicePiProfile } from "./voice-profile.js";

const port = Number.parseInt(process.env.AGENT_BRIDGE_PORT ?? "8010", 10);
const token = process.env.AGENT_BRIDGE_TOKEN;
const timeoutMs = Number.parseInt(process.env.AGENT_TIMEOUT_MS ?? "60000", 10);
const logPath = process.env.AGENT_BRIDGE_LOG_PATH ?? "logs/voice-bridge.jsonl";
const voicePiWebSearchExtension = process.env.VOICE_PI_WEB_SEARCH_EXTENSION;
const voicePiSystemPromptSource = process.env.VOICE_PI_SYSTEM_PROMPT_SOURCE;
const voicePiSdkEntry = process.env.VOICE_PI_SDK_ENTRY;
const profileHome = process.env.HOME ?? "/home/node";
const contextDir = process.env.VOICE_CONTEXT_DIR ?? process.cwd();

if (!token) {
  throw new Error("Missing AGENT_BRIDGE_TOKEN");
}

await syncVoicePiProfile({
  home: profileHome,
  extensionPath: voicePiWebSearchExtension,
  systemPromptSourcePath: voicePiSystemPromptSource,
});

const server = createBridgeServer({
  token,
  timeoutMs,
  logPath,
  streamRunner: createPiSdkStreamController({
    profileHome,
    contextDir,
    sdkEntry: voicePiSdkEntry,
  }),
});

server.listen(port, "0.0.0.0", () => {
  console.log(`agent-bridge listening on ${port}`);
});
