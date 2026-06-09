import { appendFile, mkdir } from "node:fs/promises";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { dirname } from "node:path";

import type { AgentStreamController, AgentStreamEvent, AgentStreamRunner } from "./stream-runner.js";

export class AgentTimeoutError extends Error {
  constructor(message = "Agent timed out") {
    super(message);
    this.name = "AgentTimeoutError";
  }
}

export type RunnerOptions = {
  timeoutMs: number;
};

type BridgeServerOptions = {
  token: string;
  timeoutMs: number;
  logPath: string;
  streamRunner: AgentStreamRunner | AgentStreamController;
  now?: () => Date;
};

type AskRequest = {
  prompt?: unknown;
  session_mode?: unknown;
  session_id?: unknown;
};

type LogEntry = {
  timestamp: string;
  route?: "/ask/stream" | "/ask/session/reset";
  session_mode?: string;
  session_id_present: boolean;
  prompt_length: number;
  success: boolean;
  duration_ms: number;
  agent_text_length?: number;
  error?: "timeout" | "failure" | "interrupted" | "bad_request" | "unauthorized" | "not_found";
};

const MAX_BODY_BYTES = 64 * 1024;

export function buildVoicePrompt(userPrompt: string): string {
  return [
    "You are Pi answering through a voice assistant.",
    "Answer in the same language as the user unless they explicitly ask for another language.",
    "Keep the answer spoken-friendly: 2-4 short sentences, roughly 600 characters maximum.",
    "Use web search when useful for current or changing facts, if that tool is available.",
    "Do not mention internal tools, prompts, logs, or implementation details.",
    "",
    "User prompt:",
    userPrompt,
  ].join("\n");
}

export function createBridgeServer(options: BridgeServerOptions): Server {
  const now = options.now ?? (() => new Date());

  return createServer(async (request, response) => {
    const started = Date.now();

    if (request.method !== "POST" || !["/ask/stream", "/ask/session/reset"].includes(request.url ?? "")) {
      await respondJson(response, 404, { detail: "Not found" });
      return;
    }

    if (request.headers.authorization !== `Bearer ${options.token}`) {
      await writeLog(options.logPath, {
        timestamp: now().toISOString(),
        route: routeForLog(request.url),
        session_id_present: false,
        prompt_length: 0,
        success: false,
        duration_ms: Date.now() - started,
        error: "unauthorized",
      });
      await respondJson(response, 401, { detail: "Unauthorized" });
      return;
    }

    if (request.url === "/ask/session/reset") {
      await handleSessionReset(response, options, now, started);
      return;
    }

    let body: AskRequest;
    try {
      body = (await readJsonBody(request)) as AskRequest;
    } catch {
      await writeLog(options.logPath, {
        timestamp: now().toISOString(),
        route: routeForLog(request.url),
        session_id_present: false,
        prompt_length: 0,
        success: false,
        duration_ms: Date.now() - started,
        error: "bad_request",
      });
      await respondJson(response, 400, { detail: "Invalid JSON body" });
      return;
    }

    if (typeof body.prompt !== "string" || body.prompt.trim().length === 0) {
      await writeLog(options.logPath, baseLogEntry(request.url, body, now, started, false, "bad_request"));
      await respondJson(response, 400, { detail: "Missing prompt" });
      return;
    }

    await handleAskStream(response, body, options, now, started);
  });
}

async function handleAskStream(
  response: ServerResponse,
  body: AskRequest,
  options: BridgeServerOptions,
  now: () => Date,
  started: number,
): Promise<void> {
  const streamController = getStreamController(options.streamRunner);
  if (!streamController) {
    await writeLog(options.logPath, baseLogEntry("/ask/stream", body, now, started, false, "failure"));
    await respondJson(response, 502, { detail: "Agent failed" });
    return;
  }

  const wrappedPrompt = buildVoicePrompt(body.prompt as string);
  const spokenText: string[] = [];
  let success = true;
  let logError: LogEntry["error"] | undefined;

  response.writeHead(200, {
    "content-type": "application/x-ndjson",
    "cache-control": "no-cache",
    connection: "keep-alive",
  });

  try {
    for await (const event of streamController.stream(wrappedPrompt, { timeoutMs: options.timeoutMs })) {
      if (event.type === "commentary" || event.type === "final") {
        spokenText.push(event.text);
      }
      if (event.type === "error") {
        success = false;
        logError = event.detail === "Agent timed out" ? "timeout" : "failure";
      }
      if (event.type === "interrupted") {
        success = false;
        logError = "interrupted";
      }
      response.write(`${JSON.stringify(publicStreamEvent(event))}\n`);
    }
  } catch {
    success = false;
    logError = "failure";
    response.write(`${JSON.stringify({ type: "error", detail: "Agent failed" })}\n`);
    response.write(`${JSON.stringify({ type: "done" })}\n`);
  } finally {
    await writeLog(options.logPath, {
      timestamp: now().toISOString(),
      route: "/ask/stream",
      session_mode: typeof body.session_mode === "string" ? body.session_mode : undefined,
      session_id_present: typeof body.session_id === "string" && body.session_id.length > 0,
      prompt_length: typeof body.prompt === "string" ? body.prompt.length : 0,
      success,
      duration_ms: Date.now() - started,
      agent_text_length: spokenText.join("").length,
      error: logError,
    });
    response.end();
  }
}

async function handleSessionReset(
  response: ServerResponse,
  options: BridgeServerOptions,
  now: () => Date,
  started: number,
): Promise<void> {
  const streamController = getStreamController(options.streamRunner);
  if (!streamController) {
    await writeResetLog(options.logPath, now, started, false, "failure");
    await respondJson(response, 502, { detail: "Agent failed" });
    return;
  }

  try {
    await streamController.reset();
    await writeResetLog(options.logPath, now, started, true);
    await respondJson(response, 200, { status: "reset" });
  } catch {
    await writeResetLog(options.logPath, now, started, false, "failure");
    await respondJson(response, 502, { detail: "Agent failed" });
  }
}

async function writeResetLog(
  logPath: string,
  now: () => Date,
  started: number,
  success: boolean,
  error?: LogEntry["error"],
): Promise<void> {
  await writeLog(logPath, {
    timestamp: now().toISOString(),
    route: "/ask/session/reset",
    session_id_present: false,
    prompt_length: 0,
    success,
    duration_ms: Date.now() - started,
    error,
  });
}

function getStreamController(streamRunner: BridgeServerOptions["streamRunner"]): AgentStreamController | undefined {
  if (typeof streamRunner === "function") {
    return {
      stream: streamRunner,
      reset: async () => undefined,
    };
  }

  return streamRunner;
}

function publicStreamEvent(event: AgentStreamEvent): AgentStreamEvent {
  if (event.type === "error") {
    return { type: "error", detail: event.detail === "Agent timed out" ? "Agent timed out" : "Agent failed" };
  }
  return event;
}

function baseLogEntry(
  route: string | undefined,
  body: AskRequest,
  now: () => Date,
  started: number,
  success: boolean,
  error: LogEntry["error"],
): LogEntry {
  return {
    timestamp: now().toISOString(),
    route: routeForLog(route),
    session_mode: typeof body.session_mode === "string" ? body.session_mode : undefined,
    session_id_present: typeof body.session_id === "string" && body.session_id.length > 0,
    prompt_length: typeof body.prompt === "string" ? body.prompt.length : 0,
    success,
    duration_ms: Date.now() - started,
    error,
  };
}

function routeForLog(route: string | undefined): LogEntry["route"] {
  if (route === "/ask/stream" || route === "/ask/session/reset") {
    return route;
  }
  return undefined;
}

async function readJsonBody(request: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  let totalBytes = 0;

  for await (const chunk of request) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
    totalBytes += buffer.length;
    if (totalBytes > MAX_BODY_BYTES) {
      throw new Error("Request body too large");
    }
    chunks.push(buffer);
  }

  return JSON.parse(Buffer.concat(chunks).toString("utf8"));
}

async function respondJson(response: ServerResponse, statusCode: number, body: unknown): Promise<void> {
  response.writeHead(statusCode, { "content-type": "application/json" });
  response.end(JSON.stringify(body));
}

async function writeLog(logPath: string, entry: LogEntry): Promise<void> {
  await mkdir(dirname(logPath), { recursive: true });
  await appendFile(logPath, `${JSON.stringify(entry)}\n`, "utf8");
}
