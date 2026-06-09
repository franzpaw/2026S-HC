import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { createWebSearchTool, rewriteNativeWebSearchTool } from "./web-search.ts";

export default function voiceCodexWebSearch(pi: ExtensionAPI) {
  pi.registerTool(createWebSearchTool());

  pi.on("before_provider_request", async (event, ctx) => {
    return rewriteNativeWebSearchTool(event.payload, ctx);
  });
}
