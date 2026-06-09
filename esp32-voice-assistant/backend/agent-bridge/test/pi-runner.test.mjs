import assert from "node:assert/strict";
import { test } from "node:test";

import { VOICE_PI_TOOLS } from "../dist/pi-runner.js";

test("voice Pi runner allows read-only context tools and native web_search", () => {
  assert.deepEqual(VOICE_PI_TOOLS, ["read", "grep", "find", "ls", "web_search"]);
});
