// Punchline: lobby -> pack vote -> countdown -> ring-paired write stage. Exercises
// selectGame + ready/vote and the pairing topology (Tasks 7-9 extend this file for
// the write/vote/reveal/Last-Lash flow).
import assert from "node:assert/strict";
import { newEngine, lastToWs } from "./harness-lib.mjs";

const PL = 200;
const e = await newEngine();
e.reset();
e.join(1, "ALICE"); e.join(2, "BOB"); e.join(3, "CARA"); e.join(4, "DAVE");
e.selectGame(PL);
e.contentClear();
e.contentPack(PL, "Test");
e.contentItem(JSON.stringify({ prompt: "Worst kindergarten name" }));
e.contentItem(JSON.stringify({ prompt: "Thing you should never say in a resume" }));
e.contentItem(JSON.stringify({ prompt: "A bad excuse for being late" }));
e.contentItem(JSON.stringify({ prompt: "Worst superhero power" }));

e.input(1, { t: "ready", ready: true });
e.input(2, { t: "ready", ready: true });
e.input(3, { t: "ready", ready: true });
let out = e.input(4, { t: "ready", ready: true });
for (let ms = 1000; ms <= 4000; ms += 1000) out = out.concat(e.tick(ms));

// Every one of the 4 players should now be in the "write" stage with exactly 2
// prompts (n:0 and n:1), and every prompt text should be non-empty.
const seen = {};
for (const pid of [1, 2, 3, 4]) {
  const m = lastToWs(out, pid, "punch");
  assert.equal(m.msg.phase, "play", "in play after countdown");
  assert.equal(m.msg.stage, "write", "write stage first");
  assert.equal(m.msg.round, 1, "round 1");
  assert.equal(m.msg.prompts.length, 2, "exactly 2 prompts per player (" + pid + ")");
  const ns = m.msg.prompts.map((p) => p.n).sort();
  assert.deepEqual(ns, [0, 1], "slots 0 and 1, once each");
  for (const p of m.msg.prompts) assert.ok(p.text && p.text.length > 0, "prompt has text");
  seen[pid] = m.msg.prompts;
}

console.log("punchline: lobby/pairing checks passed");
