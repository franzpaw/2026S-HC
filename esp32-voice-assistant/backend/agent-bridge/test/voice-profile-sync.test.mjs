import assert from "node:assert/strict";
import { mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

import { syncVoicePiExtension, syncVoicePiProfile } from "../dist/voice-profile.js";

const extensionPath = "/home/node/.pi/agent/extensions/codex-web-search";

test("adds the voice web search extension without replacing existing profile settings", async () => {
  const home = await mkdtemp(join(tmpdir(), "voice-pi-"));
  const settingsPath = join(home, ".pi/agent/settings.json");
  await mkdir(join(home, ".pi/agent"), { recursive: true });
  await writeFile(settingsPath, JSON.stringify({ defaultProvider: "openai-codex", packages: ["npm:pi-subagents"] }), "utf8");

  await syncVoicePiExtension({ home, extensionPath });

  assert.deepEqual(JSON.parse(await readFile(settingsPath, "utf8")), {
    defaultProvider: "openai-codex",
    packages: ["npm:pi-subagents"],
    extensions: [extensionPath],
  });
});

test("voice profile sync is idempotent", async () => {
  const home = await mkdtemp(join(tmpdir(), "voice-pi-"));
  const settingsPath = join(home, ".pi/agent/settings.json");

  await syncVoicePiExtension({ home, extensionPath });
  await syncVoicePiExtension({ home, extensionPath });

  assert.deepEqual(JSON.parse(await readFile(settingsPath, "utf8")), {
    extensions: [extensionPath],
  });
});

test("syncs the repo system prompt while preserving auth and settings", async () => {
  const home = await mkdtemp(join(tmpdir(), "voice-pi-"));
  const sourcePromptPath = join(home, "repo-SYSTEM.md");
  const agentDir = join(home, ".pi/agent");
  const settingsPath = join(agentDir, "settings.json");
  const authPath = join(agentDir, "auth.json");
  const systemPath = join(agentDir, "SYSTEM.md");

  await mkdir(agentDir, { recursive: true });
  await writeFile(sourcePromptPath, "repo voice prompt\n", "utf8");
  await writeFile(systemPath, "old profile prompt\n", "utf8");
  await writeFile(authPath, JSON.stringify({ refreshToken: "local-secret" }), "utf8");
  await writeFile(settingsPath, JSON.stringify({ defaultProvider: "openai-codex" }), "utf8");

  await syncVoicePiProfile({ home, extensionPath, systemPromptSourcePath: sourcePromptPath });

  assert.equal(await readFile(systemPath, "utf8"), "repo voice prompt\n");
  assert.deepEqual(JSON.parse(await readFile(authPath, "utf8")), { refreshToken: "local-secret" });
  assert.deepEqual(JSON.parse(await readFile(settingsPath, "utf8")), {
    defaultProvider: "openai-codex",
    extensions: [extensionPath],
  });
});

test("profile sync can run with only a system prompt source", async () => {
  const home = await mkdtemp(join(tmpdir(), "voice-pi-"));
  const sourcePromptPath = join(home, "repo-SYSTEM.md");
  const systemPath = join(home, ".pi/agent/SYSTEM.md");

  await writeFile(sourcePromptPath, "repo voice prompt\n", "utf8");

  await syncVoicePiProfile({ home, systemPromptSourcePath: sourcePromptPath });

  assert.equal(await readFile(systemPath, "utf8"), "repo voice prompt\n");
});

test("profile sync leaves the system prompt alone when source already matches the profile path", async () => {
  const home = await mkdtemp(join(tmpdir(), "voice-pi-"));
  const systemPath = join(home, ".pi/agent/SYSTEM.md");

  await mkdir(join(home, ".pi/agent"), { recursive: true });
  await writeFile(systemPath, "repo voice prompt\n", "utf8");

  await syncVoicePiProfile({ home, systemPromptSourcePath: systemPath });

  assert.equal(await readFile(systemPath, "utf8"), "repo voice prompt\n");
});
