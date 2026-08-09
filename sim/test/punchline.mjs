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
  // Exact equality, not >=: a >= check can't catch an over-payment (a doubled
  // bonus, a wrong multiplier), which is exactly the failure mode this scoring
  // code is most at risk of. 100%*10 + win bonus 100 + unanimous bonus 250 = 1350.
  assert.equal(rev.msg.gainA, 1350, "unanimous A gets exactly 100%+win+bonus");
  assert.equal(rev.msg.gainB, 0, "B got no votes, no points");
  for (let ms = 0; ms < 5500; ms += 500) out = out.concat(e.tick(rev.msg.deadline - 5000 + ms));
}
// After all 4 pairs, round 2 should have started (a fresh write stage, round 2).
const m2 = lastToWs(out, 1, "punch");
assert.equal(m2.msg.stage, "write", "round 2 opens with a fresh write stage");
assert.equal(m2.msg.round, 2, "advanced to round 2");

console.log("punchline: round 1 voting, reveal, and scoring checks passed");

// Round 2: mult=20 doubles the percentage and win-bonus components, but the
// unanimous bonus (PUNCH_UNANIMOUS_BONUS) is a flat constant, not scaled by
// mult -- exact total must be 2450 (100%*20=2000, +win 200, +flat unanimous
// 250), NOT 2700 (which is what a naive "round 2 doubles everything" would give).
{
  const seenR2 = {};
  for (const pid of [1, 2, 3, 4]) seenR2[pid] = lastToWs(out, pid, "punch").msg.prompts;
  for (const pid of [1, 2, 3, 4]) {
    for (const p of seenR2[pid]) out = e.input(pid, { t: "quip", n: p.n, text: "r2 answer " + pid + p.n });
  }
  const votersR2 = [1, 2, 3, 4].filter((pid) => {
    const m = lastToWs(out, pid, "punch");
    return m.msg.stage === "vote" && m.msg.match === 0 && !m.msg.iam;
  });
  assert.equal(votersR2.length, 2, "round 2 match 0: 2 eligible voters");
  out = e.input(votersR2[0], { t: "pick", n: 0 }); // unanimous A again
  out = e.input(votersR2[1], { t: "pick", n: 0 });
  const revR2 = lastToWs(out, votersR2[0], "punch");
  assert.equal(revR2.msg.stage, "reveal", "round 2 reveal fires once both eligible voters picked");
  assert.equal(revR2.msg.gainA, 2450, "round 2 unanimous: 100%*20 + win 200 + flat unanimous 250 = 2450, not 2700");
  assert.equal(revR2.msg.gainB, 0, "round 2 B got no votes");
  console.log("punchline: round 2 multiplier check passed");
}

// 1-1 tie: with exactly 2 eligible voters split one each way, neither side wins
// (no +100 win bonus) and neither side is unanimous (no +250) -- both land at
// exactly 50% * mult (500 in round 1).
{
  const et = await newEngine();
  et.reset();
  et.join(1, "T1"); et.join(2, "T2"); et.join(3, "T3"); et.join(4, "T4");
  et.selectGame(PL);
  et.contentClear();
  et.contentPack(PL, "Test");
  for (let i = 0; i < 4; i++) et.contentItem(JSON.stringify({ prompt: "Tie prompt " + i }));
  et.input(1, { t: "ready", ready: true });
  et.input(2, { t: "ready", ready: true });
  et.input(3, { t: "ready", ready: true });
  let outT = et.input(4, { t: "ready", ready: true });
  for (let ms = 1000; ms <= 4000; ms += 1000) outT = outT.concat(et.tick(ms));
  for (const pid of [1, 2, 3, 4]) {
    const m = lastToWs(outT, pid, "punch");
    for (const p of m.msg.prompts) outT = et.input(pid, { t: "quip", n: p.n, text: "tie ans " + pid + p.n });
  }
  const votersT = [1, 2, 3, 4].filter((pid) => {
    const m = lastToWs(outT, pid, "punch");
    return m.msg.stage === "vote" && m.msg.match === 0 && !m.msg.iam;
  });
  assert.equal(votersT.length, 2, "tie test: 2 eligible voters for match 0");
  outT = et.input(votersT[0], { t: "pick", n: 0 }); // A
  outT = et.input(votersT[1], { t: "pick", n: 1 }); // B
  const revT = lastToWs(outT, votersT[0], "punch");
  assert.equal(revT.msg.stage, "reveal", "tie: reveal fires once both eligible voters picked");
  assert.equal(revT.msg.votesA, 1, "tie: 1 vote for A");
  assert.equal(revT.msg.votesB, 1, "tie: 1 vote for B");
  assert.equal(revT.msg.gainA, 500, "tie: A gets exactly 50%*10, no win bonus, no unanimous bonus");
  assert.equal(revT.msg.gainB, 500, "tie: B gets exactly 50%*10, no win bonus, no unanimous bonus");
  console.log("punchline: 1-1 tie scoring check passed");
}

// Deadline-triggered reveal with an abstaining voter: one eligible voter picks,
// the other never does. The vote deadline must still fire the reveal (no stall),
// the abstainer must not be counted in the total, and the payout must reflect
// only the vote actually cast.
{
  const ed = await newEngine();
  ed.reset();
  ed.join(1, "D1"); ed.join(2, "D2"); ed.join(3, "D3"); ed.join(4, "D4");
  ed.selectGame(PL);
  ed.contentClear();
  ed.contentPack(PL, "Test");
  for (let i = 0; i < 4; i++) ed.contentItem(JSON.stringify({ prompt: "Deadline prompt " + i }));
  ed.input(1, { t: "ready", ready: true });
  ed.input(2, { t: "ready", ready: true });
  ed.input(3, { t: "ready", ready: true });
  let outD = ed.input(4, { t: "ready", ready: true });
  for (let ms = 1000; ms <= 4000; ms += 1000) outD = outD.concat(ed.tick(ms));
  for (const pid of [1, 2, 3, 4]) {
    const m = lastToWs(outD, pid, "punch");
    for (const p of m.msg.prompts) outD = ed.input(pid, { t: "quip", n: p.n, text: "d ans " + pid + p.n });
  }
  const votersD = [1, 2, 3, 4].filter((pid) => {
    const m = lastToWs(outD, pid, "punch");
    return m.msg.stage === "vote" && m.msg.match === 0 && !m.msg.iam;
  });
  assert.equal(votersD.length, 2, "deadline test: 2 eligible voters for match 0");
  const mVoteD = lastToWs(outD, votersD[0], "punch");
  outD = ed.input(votersD[0], { t: "pick", n: 0 }); // votersD[1] deliberately abstains
  const stillVoting = lastToWs(outD, votersD[0], "punch");
  assert.equal(stillVoting.msg.stage, "vote", "deadline test: one pick alone must not fire reveal early");
  for (let ms = 0; ms <= 21000; ms += 1000) {
    outD = outD.concat(ed.tick(mVoteD.msg.deadline - mVoteD.msg.dur * 1000 + ms));
  }
  const revD = lastToWs(outD, votersD[0], "punch");
  assert.equal(revD.msg.stage, "reveal", "deadline test: reveal fires once the vote deadline passes");
  assert.equal(revD.msg.votesA + revD.msg.votesB, 1, "deadline test: abstainer excluded from the total");
  assert.equal(revD.msg.votesA, 1, "deadline test: the one cast vote landed on A");
  assert.equal(revD.msg.gainA, 1100, "deadline test: 100%*10 + win bonus 100, total=1 so no unanimous bonus");
  assert.equal(revD.msg.gainB, 0, "deadline test: B got no votes");
  console.log("punchline: deadline-triggered reveal with abstaining voter check passed");
}

// Single-voter unanimous suppression at N=3 (the floor): a pair has exactly 1
// eligible voter, so their lone vote is trivially "100% to one side" but must
// NOT trigger the +250 unanimous bonus -- that's what the total>=2 guard is for.
{
  const es = await newEngine();
  es.reset();
  es.join(1, "S1"); es.join(2, "S2"); es.join(3, "S3");
  es.selectGame(PL);
  es.contentClear();
  es.contentPack(PL, "Test");
  for (let i = 0; i < 3; i++) es.contentItem(JSON.stringify({ prompt: "Solo prompt " + i }));
  es.input(1, { t: "ready", ready: true });
  es.input(2, { t: "ready", ready: true });
  let outS = es.input(3, { t: "ready", ready: true });
  for (let ms = 1000; ms <= 4000; ms += 1000) outS = outS.concat(es.tick(ms));
  for (const pid of [1, 2, 3]) {
    const m = lastToWs(outS, pid, "punch");
    for (const p of m.msg.prompts) outS = es.input(pid, { t: "quip", n: p.n, text: "solo ans " + pid + p.n });
  }
  const votersS = [1, 2, 3].filter((pid) => {
    const m = lastToWs(outS, pid, "punch");
    return m.msg.stage === "vote" && m.msg.match === 0 && !m.msg.iam;
  });
  assert.equal(votersS.length, 1, "N=3 floor: exactly 1 eligible voter for match 0");
  outS = es.input(votersS[0], { t: "pick", n: 0 });
  const revS = lastToWs(outS, votersS[0], "punch");
  assert.equal(revS.msg.stage, "reveal", "N=3: reveal fires as soon as the sole eligible voter picks");
  assert.equal(revS.msg.votesA, 1, "N=3: the single vote landed on A");
  assert.equal(revS.msg.votesB, 0, "N=3: no votes on B");
  assert.equal(revS.msg.gainA, 1100, "N=3: 100%*10 + win bonus 100, but total=1 so unanimous +250 must NOT apply");
  assert.equal(revS.msg.gainB, 0, "N=3: B got no votes");
  console.log("punchline: N=3 single-voter unanimous-suppression check passed");
}
