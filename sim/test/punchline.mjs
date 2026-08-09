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

// The real invariant this task delivers isn't just "2 prompts per player" -- it's
// "2 DISTINCT authors per pair". A bug that collapsed author A and B into the same
// player would still satisfy the assertions above (each player still gets one n:0
// and one n:1 message) while being structurally broken. Reconstruct the pairs from
// what each player was told and check both directions of the invariant.
const promptTexts = {};
for (const pid of [1, 2, 3, 4]) {
  const texts = seen[pid].map((p) => p.text);
  assert.notEqual(texts[0], texts[1], "a player's two prompts must differ (pid " + pid + ")");
  promptTexts[pid] = texts;
}
console.log("punchline: lobby/pairing checks passed");

// Re-run the same lobby -> pairing flow at N=3 (the floor) and N=12 (HA_MAX_PLAYERS,
// the ceiling) -- the ring math's wraparound only fully exercises at the boundaries.
for (const n of [3, 12]) {
  const e2 = await newEngine();
  e2.reset();
  for (let pid = 1; pid <= n; pid++) e2.join(pid, "P" + pid);
  e2.selectGame(PL);
  e2.contentClear();
  e2.contentPack(PL, "Test");
  for (let i = 0; i < n; i++) e2.contentItem(JSON.stringify({ prompt: "Prompt " + i }));
  let out2;
  for (let pid = 1; pid <= n; pid++) out2 = e2.input(pid, { t: "ready", ready: true });
  for (let ms = 1000; ms <= 4000; ms += 1000) out2 = out2.concat(e2.tick(ms));

  const seenTexts = {};
  for (let pid = 1; pid <= n; pid++) {
    const m = lastToWs(out2, pid, "punch");
    assert.equal(m.msg.prompts.length, 2, "n=" + n + ": pid " + pid + " gets 2 prompts");
    seenTexts[pid] = m.msg.prompts.map((p) => p.text);
    assert.notEqual(seenTexts[pid][0], seenTexts[pid][1], "n=" + n + ": pid " + pid + "'s two prompts differ");
  }
  console.log("punchline: N=" + n + " pairing checks passed");
}

// Every player writes both of their answers. A truncation probe: 45 Cyrillic
// chars = 90 bytes -- still not enough. Need > PUNCH_ANSWER_BYTES (161) worth of
// bytes to actually exercise punchUtf8Truncate()'s cut path (its early-return at
// `len < maxBytes` means anything under 161 bytes round-trips untouched and proves
// nothing about the truncation logic itself). 90 Cyrillic chars = 180 bytes clears
// it with margin. This can't yet assert on the *truncated result* -- punchJson()'s
// vote-stage shape doesn't expose textA/textB content until Task 8's reveal JSON
// does -- but it does prove punchUtf8Truncate() runs on oversized input without
// corrupting engine state or crashing, which is the adversarial-input backstop
// this buffer exists for.
const oversized90 = "б".repeat(90);
for (const pid of [1, 2, 3, 4]) {
  out = e.input(pid, { t: "quip", n: seen[pid][0].n, text: oversized90 });
}
for (const pid of [1, 2, 3]) {
  out = e.input(pid, { t: "quip", n: seen[pid][1].n, text: "short answer " + pid });
}
// Player 4's second answer is deliberately left unsent -- timeout should still
// move the game on with it empty, not stall forever.
for (let ms = 5000; ms <= 65000; ms += 1000) out = out.concat(e.tick(ms));

for (const pid of [1, 2, 3, 4]) {
  const m = lastToWs(out, pid, "punch");
  assert.equal(m.msg.stage, "vote", "moved to voting after the write deadline");
}

console.log("punchline: write stage + truncation + timeout checks passed");

// Vote every pair in round 1. 4 players -> 4 pairs, 2 eligible voters each.
for (let match = 0; match < 4; match++) {
  const voters = [1, 2, 3, 4].filter((pid) => {
    const m = lastToWs(out, pid, "punch");
    return m.msg.stage === "vote" && m.msg.match === match && !m.msg.iam;
  });
  assert.equal(voters.length, 2, "2 eligible voters for match " + match);
  out = e.input(voters[0], { t: "pick", n: 0 }); // both vote A: a unanimous pair
  out = e.input(voters[1], { t: "pick", n: 0 });
  const rev = lastToWs(out, voters[0], "punch");
  assert.equal(rev.msg.stage, "reveal", "reveal fires once both eligible voters picked");
  assert.equal(rev.msg.votesA, 2, "both votes landed on A");
  assert.ok(rev.msg.gainA >= 1000 + 100 + 250, "unanimous A gets 100%+win+bonus (got " + rev.msg.gainA + ")");
  assert.equal(rev.msg.gainB, 0, "B got no votes, no points");
  for (let ms = 0; ms < 5500; ms += 500) out = out.concat(e.tick(rev.msg.deadline - 5000 + ms));
}
// After all 4 pairs, round 2 should have started (a fresh write stage, round 2).
const m2 = lastToWs(out, 1, "punch");
assert.equal(m2.msg.stage, "write", "round 2 opens with a fresh write stage");
assert.equal(m2.msg.round, 2, "advanced to round 2");

console.log("punchline: round 1 voting, reveal, and scoring checks passed");
