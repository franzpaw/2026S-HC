import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { test } from "node:test";

const execFileAsync = promisify(execFile);
const backendRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const repoRoot = resolve(backendRoot, "..");
const deployRoot = resolve(backendRoot, "deploy");

async function composeConfig() {
  const { stdout } = await execFileAsync("docker", ["compose", "config", "--format", "json"], {
    cwd: deployRoot,
    env: {
      ...process.env,
      AGENT_BRIDGE_TOKEN: "test-agent-token",
      AGENT_BRIDGE_PORT: "8010",
      AGENT_TIMEOUT_MS: "60000",
    },
    maxBuffer: 1024 * 1024,
  });
  return JSON.parse(stdout);
}

test("docker compose defines an internal agent-bridge runtime", async () => {
  const config = await composeConfig();
  const service = config.services["agent-bridge"];

  assert.ok(service, "agent-bridge service should exist");
  assert.deepEqual(service.ports ?? [], [], "agent-bridge must not publish a host port");
  assert.equal(service.user, "1000:1000");
  assert.equal(service.environment.AGENT_BRIDGE_TOKEN, "test-agent-token");
  assert.equal(service.environment.AGENT_BRIDGE_PORT, "8010");
  assert.equal(service.environment.AGENT_TIMEOUT_MS, "60000");
  assert.equal(service.environment.AGENT_BRIDGE_LOG_PATH, "/home/node/.pi/logs/voice-bridge.jsonl");
  assert.equal(service.environment.HOME, "/home/node");
  assert.equal(service.environment.VOICE_CONTEXT_DIR, "/home/node/.pi/agent");
  assert.equal(service.environment.VOICE_PI_WEB_SEARCH_EXTENSION, "/home/node/.pi/agent/extensions/codex-web-search");
  assert.equal(service.environment.VOICE_PI_SYSTEM_PROMPT_SOURCE, "/home/node/.pi/agent/SYSTEM.md");

  const mounts = service.volumes.map((volume) => ({
    source: volume.source.replaceAll("\\", "/"),
    target: volume.target,
    readOnly: volume.read_only,
  }));

  assert.ok(
    mounts.some((mount) => mount.source.endsWith("/deploy/.pi") && mount.target === "/home/node/.pi"),
    "project-local .pi runtime should be mounted",
  );
});

test("voice Pi profile and extension expectations are represented in files", async () => {
  const gitignore = await readFile(resolve(repoRoot, ".gitignore"), "utf8");
  assert.match(gitignore, /^backend\/deploy\/\.pi\/agent\/auth\.json$/m);
  assert.match(gitignore, /^backend\/deploy\/\.pi\/agent\/sessions\/$/m);
  assert.match(gitignore, /^backend\/deploy\/\.pi\/logs\/$/m);

  const settings = JSON.parse(await readFile(resolve(backendRoot, "deploy/.pi/agent/settings.json"), "utf8"));
  assert.deepEqual(settings.extensions, ["/home/node/.pi/agent/extensions/codex-web-search"]);

  const extensionPackage = JSON.parse(await readFile(resolve(backendRoot, "deploy/.pi/agent/extensions/codex-web-search/package.json"), "utf8"));
  assert.deepEqual(extensionPackage.pi.extensions, ["./index.ts"]);

  const voicePrompt = await readFile(resolve(backendRoot, "deploy/.pi/agent/SYSTEM.md"), "utf8");
  assert.match(voicePrompt, /You are Voice-Neo/);
});
