export const WEB_SEARCH_UNSUPPORTED_MESSAGE = "web_search is only available with the openai-codex provider";
const WEB_SEARCH_LOCAL_EXECUTION_MESSAGE = "web_search is a native openai-codex provider tool and should not execute locally";
const WEB_SEARCH_MULTIMODAL_CONTENT_TYPES = ["text", "image"];

type ToolPayload = {
  tools?: unknown[];
  [key: string]: unknown;
};

type ExtensionContextLike = {
  model?: { provider?: string; id?: string };
  provider?: { id?: string };
};

export function isOpenAICodexContext(ctx: ExtensionContextLike) {
  return String(ctx?.model?.provider ?? ctx?.provider?.id ?? "").toLowerCase() === "openai-codex";
}

export function supportsMultimodalNativeWebSearch(ctx: ExtensionContextLike) {
  if (!isOpenAICodexContext(ctx)) {
    return false;
  }
  return !String(ctx?.model?.id ?? "").toLowerCase().includes("spark");
}

function isWebSearchFunctionTool(tool: unknown) {
  return Boolean(
    tool &&
      typeof tool === "object" &&
      "type" in tool &&
      tool.type === "function" &&
      "name" in tool &&
      tool.name === "web_search",
  );
}

export function rewriteNativeWebSearchTool(payload: unknown, ctx: ExtensionContextLike) {
  if (!isOpenAICodexContext(ctx) || !payload || typeof payload !== "object" || !Array.isArray((payload as ToolPayload).tools)) {
    return payload;
  }

  let rewritten = false;
  const tools = ((payload as ToolPayload).tools ?? []).map((tool) => {
    if (!isWebSearchFunctionTool(tool)) {
      return tool;
    }
    rewritten = true;
    const nativeTool: Record<string, unknown> = {
      type: "web_search",
      external_web_access: true,
    };
    if (supportsMultimodalNativeWebSearch(ctx)) {
      nativeTool.search_content_types = [...WEB_SEARCH_MULTIMODAL_CONTENT_TYPES];
    }
    return nativeTool;
  });

  if (!rewritten) {
    return payload;
  }

  return {
    ...(payload as ToolPayload),
    tools,
  };
}

export function createWebSearchTool() {
  return {
    name: "web_search",
    label: "web_search",
    description: "Search the web for current information and external sources when answering the user's voice question.",
    promptSnippet: "Use web_search when the user asks about current, changing, or external information.",
    parameters: {
      type: "object",
      additionalProperties: false,
    },
    prepareArguments: () => ({}),
    async execute(_toolCallId: string, _params: unknown, _signal: AbortSignal | undefined, _onUpdate: unknown, ctx: ExtensionContextLike) {
      if (!isOpenAICodexContext(ctx)) {
        throw new Error(WEB_SEARCH_UNSUPPORTED_MESSAGE);
      }
      throw new Error(WEB_SEARCH_LOCAL_EXECUTION_MESSAGE);
    },
  };
}
