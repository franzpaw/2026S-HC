import assert from "node:assert/strict";
import { mkdtemp, readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, test } from "node:test";

import { createBridgeServer } from "../dist/app.js";
import { createSdkStreamController, createSdkStreamRunner } from "../dist/stream-runner.js";

const servers = [];

afterEach(async () => {
  await Promise.all(
    servers.splice(0).map(
      (server) => new Promise((resolve, reject) => server.close((error) => (error ? reject(error) : resolve()))),
    ),
  );
});

async function startBridge({ streamRunner, streamController, token = "test-bridge-token", timeoutMs = 60000 } = {}) {
  const logDir = await mkdtemp(join(tmpdir(), "agent-bridge-stream-test-"));
  const logPath = join(logDir, "voice-bridge.jsonl");
  const server = createBridgeServer({
    token,
    timeoutMs,
    logPath,
    streamRunner: streamController ?? streamRunner ?? (async function* () {
      yield { type: "final", text: "Hallo stream" };
      yield { type: "done" };
    }),
    now: () => new Date("2026-05-13T12:00:00.000Z"),
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  servers.push(server);
  const address = server.address();
  assert.equal(typeof address, "object");
  return { baseUrl: `http://127.0.0.1:${address.port}`, logPath };
}

async function postStream(baseUrl, body, token = "test-bridge-token") {
  return fetch(`${baseUrl}/ask/stream`, {
    method: "POST",
    headers: {
      authorization: `Bearer ${token}`,
      "content-type": "application/json",
    },
    body: JSON.stringify(body),
  });
}

async function postReset(baseUrl, token = "test-bridge-token") {
  return fetch(`${baseUrl}/ask/session/reset`, {
    method: "POST",
    headers: { authorization: `Bearer ${token}` },
  });
}

async function readJsonl(response) {
  const text = await response.text();
  return text
    .trim()
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line));
}

function textEnd(content, textSignature) {
  return {
    type: "message_update",
    assistantMessageEvent: {
      type: "text_end",
      contentIndex: 0,
      content,
      partial: { content: [{ type: "text", text: content, textSignature }] },
    },
  };
}

test("POST /ask/stream streams JSONL commentary, final, and done", async () => {
  let runnerInput;
  const { baseUrl, logPath } = await startBridge({
    streamRunner: async function* (prompt, options) {
      runnerInput = { prompt, options };
      yield { type: "commentary", text: "Ich suche kurz." };
      yield { type: "final", text: "Die Antwort ist 36." };
      yield { type: "done" };
    },
  });

  const response = await postStream(baseUrl, { prompt: "Was ist pigeoncode?" });

  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "application/x-ndjson");
  assert.deepEqual(await readJsonl(response), [
    { type: "commentary", text: "Ich suche kurz." },
    { type: "final", text: "Die Antwort ist 36." },
    { type: "done" },
  ]);
  assert.match(runnerInput.prompt, /same language/i);
  assert.match(runnerInput.prompt, /Was ist pigeoncode\?/);
  assert.equal(runnerInput.options.timeoutMs, 60000);

  const log = await readFile(logPath, "utf8");
  const entry = JSON.parse(log.trim());
  assert.equal(entry.success, true);
  assert.equal(entry.prompt_length, "Was ist pigeoncode?".length);
  assert.equal(entry.agent_text_length, "Ich suche kurz.Die Antwort ist 36.".length);
  assert.equal(log.includes("Was ist pigeoncode?"), false);
  assert.equal(log.includes("test-bridge-token"), false);
});

test("POST /ask/stream requires bearer auth", async () => {
  const { baseUrl } = await startBridge();

  const response = await fetch(`${baseUrl}/ask/stream`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ prompt: "Hallo" }),
  });

  assert.equal(response.status, 401);
  assert.deepEqual(await response.json(), { detail: "Unauthorized" });
});

test("POST /ask/stream validates prompt", async () => {
  const { baseUrl } = await startBridge();

  const response = await postStream(baseUrl, { prompt: "" });

  assert.equal(response.status, 400);
  assert.deepEqual(await response.json(), { detail: "Missing prompt" });
});

test("overlapping POST /ask/stream interrupts the old stream and lets the new stream finish", async () => {
  let firstPromptStarted;
  let finishFirstPrompt;
  const session = {
    subscribe() {
      return () => {};
    },
    async prompt(prompt) {
      if (prompt.includes("first slow prompt")) {
        firstPromptStarted();
        await new Promise((resolve) => {
          finishFirstPrompt = resolve;
        });
        return;
      }
    },
    async abort() {
      finishFirstPrompt?.();
    },
  };
  const controller = createSdkStreamController({ getSession: async () => session });
  const { baseUrl } = await startBridge({ streamController: controller });

  const firstStarted = new Promise((resolve) => {
    firstPromptStarted = resolve;
  });
  const firstResponsePromise = postStream(baseUrl, { prompt: "first slow prompt" });
  await firstStarted;

  const secondResponse = await postStream(baseUrl, { prompt: "second prompt" });
  const firstResponse = await firstResponsePromise;

  assert.deepEqual(await readJsonl(firstResponse), [{ type: "interrupted" }, { type: "done" }]);
  assert.deepEqual(await readJsonl(secondResponse), [{ type: "done" }]);
});

test("POST /ask/session/reset aborts active work and next stream uses a fresh SDK session", async () => {
  let activePromptStarted;
  let finishActivePrompt;
  let aborts = 0;
  let sessionCreates = 0;
  const controller = createSdkStreamController({
    createSession: async () => {
      sessionCreates += 1;
      return {
        subscribe() {
          return () => {};
        },
        async prompt(prompt) {
          if (prompt.includes("active prompt")) {
            activePromptStarted();
            await new Promise((resolve) => {
              finishActivePrompt = resolve;
            });
          }
        },
        async abort() {
          aborts += 1;
          finishActivePrompt?.();
        },
      };
    },
  });
  const { baseUrl } = await startBridge({ streamController: controller });

  const activeStarted = new Promise((resolve) => {
    activePromptStarted = resolve;
  });
  const activeResponsePromise = postStream(baseUrl, { prompt: "active prompt" });
  await activeStarted;

  const resetResponse = await postReset(baseUrl);
  const activeResponse = await activeResponsePromise;
  const nextResponse = await postStream(baseUrl, { prompt: "after reset" });

  assert.equal(resetResponse.status, 200);
  assert.deepEqual(await resetResponse.json(), { status: "reset" });
  assert.deepEqual(await readJsonl(activeResponse), [{ type: "interrupted" }, { type: "done" }]);
  assert.deepEqual(await readJsonl(nextResponse), [{ type: "done" }]);
  assert.equal(aborts, 1);
  assert.equal(sessionCreates, 2);
});

test("reset during SDK session creation interrupts the pending stream and uses a fresh session", async () => {
  let releaseFirstSession;
  let sessionCreates = 0;
  const controller = createSdkStreamController({
    createSession: async () => {
      sessionCreates += 1;
      if (sessionCreates === 1) {
        await new Promise((resolve) => {
          releaseFirstSession = resolve;
        });
      }
      return {
        subscribe() {
          return () => {};
        },
        async prompt() {},
      };
    },
  });
  const { baseUrl } = await startBridge({ streamController: controller });

  const pendingResponsePromise = postStream(baseUrl, { prompt: "pending session" });
  await Promise.resolve();
  const resetResponse = await postReset(baseUrl);
  releaseFirstSession();
  const pendingResponse = await pendingResponsePromise;
  const nextResponse = await postStream(baseUrl, { prompt: "fresh session" });

  assert.equal(resetResponse.status, 200);
  assert.deepEqual(await readJsonl(pendingResponse), [{ type: "interrupted" }, { type: "done" }]);
  assert.deepEqual(await readJsonl(nextResponse), [{ type: "done" }]);
  assert.equal(sessionCreates, 2);
});

test("POST /ask/session/reset requires bearer auth", async () => {
  const { baseUrl } = await startBridge();

  const response = await fetch(`${baseUrl}/ask/session/reset`, { method: "POST" });

  assert.equal(response.status, 401);
  assert.deepEqual(await response.json(), { detail: "Unauthorized" });
});

test("SDK stream runner maps commentary, final_answer, unphased final, and done", async () => {
  const commentarySignature = JSON.stringify({ v: 1, id: "commentary-id", phase: "commentary" });
  const finalSignature = JSON.stringify({ v: 1, id: "final-id", phase: "final_answer" });
  const listeners = [];
  const session = {
    subscribe(listener) {
      listeners.push(listener);
      return () => {};
    },
    async prompt() {
      listeners.forEach((listener) => listener(textEnd("Ich lese Kontext.", commentarySignature)));
      listeners.forEach((listener) => listener({ type: "message_update", assistantMessageEvent: { type: "thinking_end", content: "hidden" } }));
      listeners.forEach((listener) => listener({ type: "tool_execution_start", toolName: "read" }));
      listeners.forEach((listener) => listener(textEnd("Final mit Phase.", finalSignature)));
      listeners.forEach((listener) => listener(textEnd("Final fallback.", "legacy-text-id")));
    },
  };
  const runner = createSdkStreamRunner({ getSession: async () => session });

  const events = [];
  for await (const event of runner("Hallo", { timeoutMs: 60000 })) {
    events.push(event);
  }

  assert.deepEqual(events, [
    { type: "commentary", text: "Ich lese Kontext." },
    { type: "final", text: "Final mit Phase." },
    { type: "final", text: "Final fallback." },
    { type: "done" },
  ]);
});

test("SDK stream runner maps aborted SDK updates to interrupted events", async () => {
  const listeners = [];
  const session = {
    subscribe(listener) {
      listeners.push(listener);
      return () => {};
    },
    async prompt() {
      listeners.forEach((listener) => listener({ type: "message_update", assistantMessageEvent: { type: "error", reason: "aborted" } }));
    },
  };
  const runner = createSdkStreamRunner({ getSession: async () => session });

  const events = [];
  for await (const event of runner("Hallo", { timeoutMs: 60000 })) {
    events.push(event);
  }

  assert.deepEqual(events, [{ type: "interrupted" }, { type: "done" }]);
});

test("SDK stream runner maps SDK error updates to sanitized error events", async () => {
  const listeners = [];
  const session = {
    subscribe(listener) {
      listeners.push(listener);
      return () => {};
    },
    async prompt() {
      listeners.forEach((listener) =>
        listener({ type: "message_update", assistantMessageEvent: { type: "error", reason: "error", error: { errorMessage: "secret provider stacktrace" } } }),
      );
    },
  };
  const runner = createSdkStreamRunner({ getSession: async () => session });

  const events = [];
  for await (const event of runner("Hallo", { timeoutMs: 60000 })) {
    events.push(event);
  }

  assert.deepEqual(events, [{ type: "error", detail: "Agent failed" }, { type: "done" }]);
});

test("SDK stream runner sanitizes prompt failures", async () => {
  const session = {
    subscribe() {
      return () => {};
    },
    async prompt() {
      throw new Error("secret provider stacktrace");
    },
  };
  const runner = createSdkStreamRunner({ getSession: async () => session });

  const events = [];
  for await (const event of runner("Hallo", { timeoutMs: 60000 })) {
    events.push(event);
  }

  assert.deepEqual(events, [{ type: "error", detail: "Agent failed" }, { type: "done" }]);
});
