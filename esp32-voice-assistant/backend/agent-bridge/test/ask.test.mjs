import assert from "node:assert/strict";
import { mkdtemp, readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, test } from "node:test";

import { createBridgeServer } from "../dist/app.js";

const servers = [];

afterEach(async () => {
  await Promise.all(
    servers.splice(0).map(
      (server) => new Promise((resolve, reject) => server.close((error) => (error ? reject(error) : resolve()))),
    ),
  );
});

async function startBridge({ streamRunner, token = "test-bridge-token", timeoutMs = 60000 } = {}) {
  const logDir = await mkdtemp(join(tmpdir(), "agent-bridge-test-"));
  const logPath = join(logDir, "voice-bridge.jsonl");
  const server = createBridgeServer({
    token,
    timeoutMs,
    logPath,
    streamRunner:
      streamRunner ??
      async function* () {
        yield { type: "final", text: "Hallo von Pi" };
        yield { type: "done" };
      },
    now: () => new Date("2026-05-13T12:00:00.000Z"),
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  servers.push(server);
  const address = server.address();
  assert.equal(typeof address, "object");
  return { baseUrl: `http://127.0.0.1:${address.port}`, logPath };
}

async function postAskStream(baseUrl, body, token = "test-bridge-token") {
  return fetch(`${baseUrl}/ask/stream`, {
    method: "POST",
    headers: {
      authorization: `Bearer ${token}`,
      "content-type": "application/json",
    },
    body: JSON.stringify(body),
  });
}

test("POST /ask is removed", async () => {
  const { baseUrl } = await startBridge();

  const response = await fetch(`${baseUrl}/ask`, {
    method: "POST",
    headers: {
      authorization: "Bearer test-bridge-token",
      "content-type": "application/json",
    },
    body: JSON.stringify({ prompt: "Hallo", session_mode: "ephemeral" }),
  });

  assert.equal(response.status, 404);
});

test("POST /ask/stream streams sanitized public events and logs metadata", async () => {
  let runnerInput;
  const { baseUrl, logPath } = await startBridge({
    streamRunner: async function* (prompt, options) {
      runnerInput = { prompt, options };
      yield { type: "commentary", text: "Ich suche." };
      yield { type: "final", text: "Fertig." };
      yield { type: "done" };
    },
  });

  const response = await postAskStream(baseUrl, {
    prompt: "Was ist Rust?",
    session_mode: "persistent",
  });

  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "application/x-ndjson");
  assert.deepEqual(
    (await response.text())
      .trim()
      .split("\n")
      .map((line) => JSON.parse(line)),
    [
      { type: "commentary", text: "Ich suche." },
      { type: "final", text: "Fertig." },
      { type: "done" },
    ],
  );
  assert.match(runnerInput.prompt, /same language/i);
  assert.match(runnerInput.prompt, /2-4 short sentences/i);
  assert.match(runnerInput.prompt, /web search/i);
  assert.match(runnerInput.prompt, /Was ist Rust\?/);
  assert.equal(runnerInput.options.timeoutMs, 60000);

  const log = await readFile(logPath, "utf8");
  const entry = JSON.parse(log.trim());
  assert.equal(entry.route, "/ask/stream");
  assert.equal(entry.success, true);
  assert.equal(entry.prompt_length, "Was ist Rust?".length);
  assert.equal(entry.agent_text_length, "Ich suche.Fertig.".length);
  assert.equal(log.includes("Was ist Rust?"), false);
  assert.equal(log.includes("test-bridge-token"), false);
});

test("POST /ask/stream requires bearer auth", async () => {
  const { baseUrl } = await startBridge();

  const response = await fetch(`${baseUrl}/ask/stream`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ prompt: "Hallo", session_mode: "persistent" }),
  });

  assert.equal(response.status, 401);
  assert.deepEqual(await response.json(), { detail: "Unauthorized" });
});

test("POST /ask/stream validates JSON and prompt", async () => {
  const { baseUrl } = await startBridge();

  const invalidJson = await fetch(`${baseUrl}/ask/stream`, {
    method: "POST",
    headers: {
      authorization: "Bearer test-bridge-token",
      "content-type": "application/json",
    },
    body: "{not json",
  });
  const missingPrompt = await postAskStream(baseUrl, { prompt: "   ", session_mode: "persistent" });

  assert.equal(invalidJson.status, 400);
  assert.deepEqual(await invalidJson.json(), { detail: "Invalid JSON body" });
  assert.equal(missingPrompt.status, 400);
  assert.deepEqual(await missingPrompt.json(), { detail: "Missing prompt" });
});

test("stream errors are sanitized", async () => {
  const { baseUrl, logPath } = await startBridge({
    streamRunner: async function* () {
      yield { type: "error", detail: "secret model stderr" };
      yield { type: "done" };
    },
  });

  const response = await postAskStream(baseUrl, {
    prompt: "Hallo",
    session_mode: "persistent",
  });

  assert.equal(response.status, 200);
  assert.deepEqual(
    (await response.text())
      .trim()
      .split("\n")
      .map((line) => JSON.parse(line)),
    [
      { type: "error", detail: "Agent failed" },
      { type: "done" },
    ],
  );
  const log = await readFile(logPath, "utf8");
  assert.equal(log.includes("secret model stderr"), false);
  assert.equal(JSON.parse(log.trim()).error, "failure");
});

test("POST /ask/session/reset delegates to stream controller reset", async () => {
  let resetCalled = false;
  const { baseUrl } = await startBridge({
    streamRunner: {
      stream: async function* () {
        yield { type: "done" };
      },
      reset: async () => {
        resetCalled = true;
      },
    },
  });

  const response = await fetch(`${baseUrl}/ask/session/reset`, {
    method: "POST",
    headers: { authorization: "Bearer test-bridge-token" },
  });

  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { status: "reset" });
  assert.equal(resetCalled, true);
});
