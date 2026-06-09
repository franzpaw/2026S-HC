import { mkdir } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { AgentTimeoutError, type RunnerOptions } from "./app.js";
import { VOICE_PI_TOOLS } from "./pi-runner.js";

export type AgentStreamEvent =
  | { type: "commentary"; text: string }
  | { type: "final"; text: string }
  | { type: "error"; detail: string }
  | { type: "interrupted" }
  | { type: "done" };

export type AgentStreamRunner = (prompt: string, options: RunnerOptions) => AsyncIterable<AgentStreamEvent>;

export type AgentStreamController = {
  stream: AgentStreamRunner;
  reset: () => Promise<void>;
};

type VoicePiSession = {
  prompt(prompt: string): Promise<unknown>;
  subscribe(listener: (event: unknown) => void): () => void;
  abort?: () => Promise<unknown>;
};

type SdkStreamRunnerOptions =
  | { getSession: () => Promise<VoicePiSession>; createSession?: never }
  | { createSession: () => Promise<VoicePiSession>; getSession?: never };

type PiSdkRunnerOptions = {
  profileHome: string;
  contextDir: string;
  sdkEntry?: string;
};

type TextSignature = {
  v?: number;
  id?: string;
  phase?: "commentary" | "final_answer";
};

type ActiveRun = {
  id: symbol;
  interrupt: () => Promise<void>;
};

class AsyncEventQueue<T> implements AsyncIterable<T> {
  private readonly values: T[] = [];
  private readonly waiters: Array<(result: IteratorResult<T>) => void> = [];
  private ended = false;

  push(value: T): void {
    if (this.ended) return;

    const waiter = this.waiters.shift();
    if (waiter) {
      waiter({ value, done: false });
      return;
    }
    this.values.push(value);
  }

  end(): void {
    this.ended = true;
    while (this.waiters.length > 0) {
      this.waiters.shift()?.({ value: undefined, done: true });
    }
  }

  async *[Symbol.asyncIterator](): AsyncIterator<T> {
    while (true) {
      if (this.values.length > 0) {
        yield this.values.shift() as T;
        continue;
      }

      if (this.ended) {
        return;
      }

      const result = await new Promise<IteratorResult<T>>((resolveWaiter) => this.waiters.push(resolveWaiter));
      if (result.done) {
        return;
      }
      yield result.value;
    }
  }
}

export function createSdkStreamRunner(options: SdkStreamRunnerOptions): AgentStreamRunner {
  return createSdkStreamController(options).stream;
}

export function createSdkStreamController(options: SdkStreamRunnerOptions): AgentStreamController {
  let sessionPromise: Promise<VoicePiSession> | undefined;
  let activeRun: ActiveRun | undefined;
  let generation = 0;

  const getSession = async () => {
    if ("getSession" in options && options.getSession) {
      sessionPromise ??= options.getSession();
    } else {
      sessionPromise ??= options.createSession();
    }
    return sessionPromise;
  };

  return {
    stream: async function* runSdkStream(prompt, runnerOptions) {
      await activeRun?.interrupt();

      const queue = new AsyncEventQueue<AgentStreamEvent>();
      const runId = Symbol("voice-run");
      const runGeneration = generation;
      let session: VoicePiSession | undefined;
      let settled = false;
      let unsubscribe: () => void = () => undefined;
      let timeout: ReturnType<typeof setTimeout> | undefined;

      const finish = (events: AgentStreamEvent[]) => {
        if (settled) return;
        settled = true;
        if (timeout) clearTimeout(timeout);
        events.forEach((event) => queue.push(event));
        queue.end();
        unsubscribe();
        if (activeRun?.id === runId) {
          activeRun = undefined;
        }
      };

      const interrupt = async () => {
        finish([{ type: "interrupted" }, { type: "done" }]);
        await session?.abort?.().catch(() => undefined);
      };

      activeRun = { id: runId, interrupt };
      timeout = setTimeout(async () => {
        finish([{ type: "error", detail: "Agent timed out" }, { type: "done" }]);
        await session?.abort?.().catch(() => undefined);
      }, runnerOptions.timeoutMs);

      void (async () => {
        try {
          session = await getSession();
          if (settled || runGeneration !== generation) return;

          unsubscribe = session.subscribe((event) => {
            const streamEvent = sdkEventToBridgeEvent(event);
            if (streamEvent) {
              queue.push(streamEvent);
            }
          });

          await session.prompt(prompt);
          finish([{ type: "done" }]);
        } catch (error) {
          finish([{ type: "error", detail: error instanceof AgentTimeoutError ? "Agent timed out" : "Agent failed" }, { type: "done" }]);
        }
      })();

      for await (const event of queue) {
        yield event;
      }
    },
    reset: async () => {
      generation += 1;
      await activeRun?.interrupt();
      activeRun = undefined;
      sessionPromise = undefined;
    },
  };
}

export function sdkEventToBridgeEvent(event: unknown): AgentStreamEvent | undefined {
  if (!isRecord(event) || event.type !== "message_update") {
    return undefined;
  }

  const update = event.assistantMessageEvent;
  if (!isRecord(update)) {
    return undefined;
  }

  if (update.type === "error") {
    const reason = typeof update.reason === "string" ? update.reason : undefined;
    return reason === "aborted" ? { type: "interrupted" } : { type: "error", detail: "Agent failed" };
  }

  if (update.type !== "text_end") {
    return undefined;
  }

  const content = typeof update.content === "string" ? update.content : "";
  if (content.trim().length === 0) {
    return undefined;
  }

  const partial = isRecord(update.partial) ? update.partial : undefined;
  const partialContent = Array.isArray(partial?.content) ? partial.content : [];
  const contentIndex = typeof update.contentIndex === "number" ? update.contentIndex : -1;
  const block = partialContent[contentIndex];
  const textSignature = isRecord(block) && typeof block.textSignature === "string" ? block.textSignature : undefined;
  const phase = parseTextSignature(textSignature)?.phase;

  return phase === "commentary" ? { type: "commentary", text: content } : { type: "final", text: content };
}

export function createPiSdkStreamRunner(options: PiSdkRunnerOptions): AgentStreamRunner {
  return createPiSdkStreamController(options).stream;
}

export function createPiSdkStreamController(options: PiSdkRunnerOptions): AgentStreamController {
  return createSdkStreamController({
    createSession: () => createPersistentPiSession(options),
  });
}

async function createPersistentPiSession(options: PiSdkRunnerOptions): Promise<VoicePiSession> {
  const sdkEntry = resolve(options.sdkEntry ?? defaultPiSdkEntry());
  const agentDir = resolve(options.profileHome, ".pi/agent");
  const sessionDir = resolve(agentDir, "sessions");
  await mkdir(sessionDir, { recursive: true });

  process.env.HOME = options.profileHome;
  const sdk = await import(pathToFileURL(sdkEntry).href);
  const result = await sdk.createAgentSession({
    cwd: options.contextDir,
    agentDir,
    tools: [...VOICE_PI_TOOLS],
    sessionManager: sdk.SessionManager.create(options.contextDir, sessionDir),
  });

  return result.session as VoicePiSession;
}

function defaultPiSdkEntry(): string {
  return "/usr/local/lib/node_modules/@earendil-works/pi-coding-agent/dist/index.js";
}

function parseTextSignature(signature: string | undefined): TextSignature | undefined {
  if (!signature || !signature.startsWith("{")) {
    return undefined;
  }

  try {
    const parsed = JSON.parse(signature) as TextSignature;
    return parsed.v === 1 ? parsed : undefined;
  } catch {
    return undefined;
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}
