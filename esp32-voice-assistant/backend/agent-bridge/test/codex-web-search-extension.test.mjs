import assert from "node:assert/strict";
import { test } from "node:test";

import { createJiti } from "jiti";

const jiti = createJiti(import.meta.url);
const { isOpenAICodexContext, rewriteNativeWebSearchTool } = await jiti.import(
  "../../deploy/.pi/agent/extensions/codex-web-search/web-search.ts",
);
const extensionModule = await jiti.import("../../deploy/.pi/agent/extensions/codex-web-search/index.ts");

test("loads the Pi extension entrypoint", () => {
  assert.equal(typeof extensionModule.default, "function");
});

test("recognizes the openai-codex provider as native web search capable", () => {
  assert.equal(isOpenAICodexContext({ model: { provider: "openai-codex", id: "gpt-5.5" } }), true);
  assert.equal(isOpenAICodexContext({ provider: { id: "openai-codex" }, model: { id: "gpt-5.5" } }), true);
  assert.equal(isOpenAICodexContext({ model: { provider: "openai", id: "gpt-5.5" } }), false);
});

test("rewrites the synthetic web_search function tool to native Codex web_search", () => {
  const payload = {
    tools: [
      { type: "function", name: "web_search", parameters: { type: "object" } },
      { type: "function", name: "read" },
    ],
  };

  assert.deepEqual(rewriteNativeWebSearchTool(payload, { model: { provider: "openai-codex", id: "gpt-5.5" } }), {
    tools: [
      { type: "web_search", external_web_access: true, search_content_types: ["text", "image"] },
      { type: "function", name: "read" },
    ],
  });
});

test("does not rewrite web_search outside openai-codex", () => {
  const payload = { tools: [{ type: "function", name: "web_search" }] };

  assert.equal(rewriteNativeWebSearchTool(payload, { model: { provider: "openai", id: "gpt-5.5" } }), payload);
});

test("omits multimodal search content types for spark models", () => {
  const payload = { tools: [{ type: "function", name: "web_search" }] };

  assert.deepEqual(rewriteNativeWebSearchTool(payload, { model: { provider: "openai-codex", id: "gpt-5.5-spark" } }), {
    tools: [{ type: "web_search", external_web_access: true }],
  });
});
