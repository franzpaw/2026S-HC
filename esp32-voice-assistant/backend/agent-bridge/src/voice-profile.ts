import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";

interface VoicePiSettings {
  extensions?: unknown;
  [key: string]: unknown;
}

interface SyncVoicePiExtensionOptions {
  home: string;
  extensionPath: string;
}

interface SyncVoicePiProfileOptions {
  home: string;
  extensionPath?: string;
  systemPromptSourcePath?: string;
}

async function readSettings(settingsPath: string): Promise<VoicePiSettings> {
  try {
    return JSON.parse(await readFile(settingsPath, "utf8")) as VoicePiSettings;
  } catch (error) {
    if (error && typeof error === "object" && "code" in error && error.code === "ENOENT") {
      return {};
    }
    throw error;
  }
}

export async function syncVoicePiExtension({ home, extensionPath }: SyncVoicePiExtensionOptions): Promise<void> {
  if (!extensionPath) {
    return;
  }

  const settingsPath = join(home, ".pi", "agent", "settings.json");
  const settings = await readSettings(settingsPath);
  const extensions = Array.isArray(settings.extensions) ? settings.extensions.filter((entry): entry is string => typeof entry === "string") : [];

  if (!extensions.includes(extensionPath)) {
    extensions.push(extensionPath);
  }

  settings.extensions = extensions;
  await mkdir(dirname(settingsPath), { recursive: true });
  await writeFile(settingsPath, `${JSON.stringify(settings, null, 2)}\n`, "utf8");
}

async function syncVoicePiSystemPrompt(home: string, sourcePath: string): Promise<void> {
  if (!sourcePath) {
    return;
  }

  const systemPath = join(home, ".pi", "agent", "SYSTEM.md");
  if (resolve(sourcePath) === resolve(systemPath)) {
    return;
  }

  const systemPrompt = await readFile(sourcePath, "utf8");
  await mkdir(dirname(systemPath), { recursive: true });
  await writeFile(systemPath, systemPrompt, "utf8");
}

export async function syncVoicePiProfile({ home, extensionPath, systemPromptSourcePath }: SyncVoicePiProfileOptions): Promise<void> {
  if (extensionPath) {
    await syncVoicePiExtension({ home, extensionPath });
  }

  if (systemPromptSourcePath) {
    await syncVoicePiSystemPrompt(home, systemPromptSourcePath);
  }
}
