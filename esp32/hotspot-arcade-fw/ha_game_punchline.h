#pragma once
// Punchline: Quiplash-style write-then-vote party game. #included once from
// ha_games.h, inside the private section of class Engine -- every function here
// relies on Engine's private members (_p[], connectedCount(), pushAll(),
// ha_json_escape(), the Party helpers, ...) exactly like the built-in games.
//
// Rounds 1-2: connected players are arranged in a ring (player i writes into pair
// i as author A, and into pair i-1 as author B), so every prompt has exactly two
// authors and every player writes exactly two answers. Pairs are then voted on one
// at a time (matchIdx); everyone except the pair's two authors votes A or B.
// Round 3 ("Last Lash"): one prompt to everyone, and everyone gets 3 votes to
// spread across other players' answers.

// PUNCH_* size/timing constants (PUNCH_ANSWER_CHARS, PUNCH_ANSWER_BYTES,
// PUNCH_PROMPT_BYTES, PUNCH_ROUNDS, PUNCH_WRITE_SECS, PUNCH_VOTE_SECS,
// PUNCH_LASH_VOTE_SECS, PUNCH_REVEAL_MS, PUNCH_LASH_REVEAL_MS, PUNCH_LASH_VOTES,
// PUNCH_UNANIMOUS_BONUS) live in ha_games.h, alongside every other game's
// constants -- onInput()'s "quip" branch (textually before this header's
// #include point) needs PUNCH_ANSWER_BYTES, and #define is a preprocessor
// symbol, not a class member, so it must be defined before that point of use.

// Truncate a UTF-8 C string in place to at most maxBytes-1 bytes without splitting
// a multi-byte codepoint. Continuation bytes are 10xxxxxx (0x80..0xBF); walking
// back from the byte limit to the nearest lead byte finds a safe cut point. Plain
// strlcpy() truncates on raw bytes, which can leave a dangling continuation byte
// that renders as mojibake -- the same bug class v1.5.0 fixed in Word Scramble.
// In practice ha_json_str() already caps input well under PUNCH_ANSWER_BYTES (the
// web client enforces the 40-char limit before sending), so this is the backstop
// for a hostile or malformed WebSocket frame, not the everyday path.
static inline void punchUtf8Truncate(char* s, size_t maxBytes) {
    size_t len = strlen(s);
    if(len < maxBytes) return;
    size_t cut = maxBytes - 1;
    while(cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    s[cut] = '\0';
}

// One prompt slot in a round-1/2 pair: its two authors and their answers.
struct PunchPair {
    uint8_t a, b; // author pids, 0 = empty slot
    char prompt[PUNCH_PROMPT_BYTES];
    char textA[PUNCH_ANSWER_BYTES];
    char textB[PUNCH_ANSWER_BYTES];
    bool inA, inB; // answer submitted (vs. left at "")
    uint8_t votesA, votesB;
};

struct PunchState {
    Party pt;
    WordPack packs[TRIVIA_MAX_TOPICS]; // each words[] entry is one prompt string
    uint8_t packCount;
    int8_t vote[HA_MAX_PLAYERS + 1]; // pack index, -1 = not voted
    uint8_t pack; // chosen pack (locked when the game starts)
    uint16_t promptSeq; // advances the prompts drawn across rounds

    uint8_t stage; // 0 write, 1 vote (both rounds 1-2 and the Last Lash)

    // ---- rounds 1-2 ----
    uint8_t pairCount; // == connectedCount() at round start
    PunchPair pairs[HA_MAX_PLAYERS];
    uint8_t matchIdx; // which pair is on screen right now
    bool pairRevealing; // false = voting/writing, true = showing a result
    int8_t pick[HA_MAX_PLAYERS + 1]; // this pair's vote: 0=A, 1=B, -1=none

    // ---- round 3, "Last Lash" ----
    char lashPrompt[PUNCH_PROMPT_BYTES];
    char lashText[HA_MAX_PLAYERS + 1][PUNCH_ANSWER_BYTES];
    bool lashIn[HA_MAX_PLAYERS + 1];
    uint8_t lashVotesUsed[HA_MAX_PLAYERS + 1];
    bool lashVotedFor[HA_MAX_PLAYERS + 1][HA_MAX_PLAYERS + 1]; // [voter][target]
    uint16_t lashTally[HA_MAX_PLAYERS + 1]; // votes received, by target pid

    int gained[HA_MAX_PLAYERS + 1]; // points earned this round, for the reveal UI
};

// ---------- punchline (write-then-vote) ----------

void punchClear() {
    partyClear(_punch.pt);
    _punch.pack = 0;
    _punch.promptSeq = 0;
    _punch.stage = 0;
    _punch.pairCount = 0;
    _punch.matchIdx = 0;
    _punch.pairRevealing = false;
    for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
        _punch.vote[i] = -1;
        _punch.pick[i] = -1;
        _punch.lashIn[i] = false;
        _punch.lashVotesUsed[i] = 0;
        _punch.lashTally[i] = 0;
        _punch.gained[i] = 0;
        _punch.lashText[i][0] = '\0';
        for(int j = 0; j <= HA_MAX_PLAYERS; j++) _punch.lashVotedFor[i][j] = false;
    }
    for(int i = 0; i < HA_MAX_PLAYERS; i++) _punch.pairs[i] = PunchPair{};
    _punch.lashPrompt[0] = '\0';
}

void punchReady(uint8_t pid, bool val) {
    if(_active != HA_GAME_PUNCHLINE) return;
    if(_punch.pt.phase != 0 && _punch.pt.phase != 4) return;
    if(_punch.pt.phase == 4 && val) punchClear();
    _punch.pt.ready[pid] = val;
    punchCheckStart();
    pushAll();
}

void punchVote(uint8_t pid, int pack) {
    if(_active != HA_GAME_PUNCHLINE || _punch.pt.phase != 0) return;
    if(pack < 0 || pack >= _punch.packCount) return;
    _punch.vote[pid] = (int8_t)pack;
    pushAll();
}

// Same as spectrumCheckStart(), plus a floor of 3 connected players: ring-pairing
// needs at least 3 distinct people to produce a sensible pairing.
void punchCheckStart() {
    if(_punch.packCount == 0) return;
    Party& pt = _punch.pt;
    if(pt.phase == 0 && partyAllReady(pt) && connectedCount() >= 3) {
        pt.phase = 1;
        pt.countdownEnd = millis() + (uint32_t)PARTY_COUNTDOWN * 1000;
        pt.lastSec = -1;
    } else if(pt.phase == 1 && (!partyAllReady(pt) || connectedCount() < 3)) {
        pt.phase = 0;
    }
}

// Mirrors spectrumWinningPack(): most pre-game votes wins, ties broken at random,
// an untallied vote (nobody voted) picks uniformly at random.
int punchWinningPack() {
    if(_punch.packCount == 0) return 0;
    int votes[TRIVIA_MAX_TOPICS] = {0};
    int total = 0;
    for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
        if(_p[i].used && _punch.vote[i] >= 0 && _punch.vote[i] < _punch.packCount) {
            votes[_punch.vote[i]]++;
            total++;
        }
    if(total == 0) return (int)random(_punch.packCount);
    int best = 0;
    for(int i = 1; i < _punch.packCount; i++)
        if(votes[i] > votes[best]) best = i;
    int tie[TRIVIA_MAX_TOPICS], tn = 0;
    for(int i = 0; i < _punch.packCount; i++)
        if(votes[i] == votes[best]) tie[tn++] = i;
    return tie[(int)random(tn)];
}

// Map a punchline pack file's {prompt} key into the current pack.
bool punchLoadItem(const char* json) {
    if(_punch.packCount == 0) return false;
    WordPack& p = _punch.packs[_punch.packCount - 1];
    if(p.count >= PACK_MAX_ITEMS) return false;
    char buf[PUNCH_PROMPT_BYTES];
    if(!ha_json_str(json, "prompt", buf, sizeof(buf)) || !buf[0]) return false;
    p.words[p.count++] = buf;
    return true;
}

// Ring-pair every connected player: player order[i] authors pair i (as A) and
// pair i-1 (as B), so N players produce N pairs and every player writes exactly
// two answers. `order` is scanned fresh (ascending pid) each round rather than
// stored, matching how spectrumPickPsychic() enumerates players.
void punchBuildPairs(uint32_t now) {
    Party& pt = _punch.pt;
    WordPack& pk = _punch.packs[_punch.pack];
    if(pk.count == 0) {
        pt.phase = 4;
        pushAll();
        return;
    }

    uint8_t order[HA_MAX_PLAYERS];
    uint8_t n = 0;
    for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
        if(_p[i].used) order[n++] = i;
    if(n < 3) { // roster dropped below the floor mid-game
        pt.phase = 4;
        pushAll();
        return;
    }

    _punch.pairCount = n;
    for(uint8_t i = 0; i < n; i++) {
        PunchPair& pr = _punch.pairs[i];
        pr = PunchPair{};
        pr.a = order[i];
        pr.b = order[(i + 1) % n];
        const String& p = pk.words[(_punch.promptSeq + i) % pk.count];
        strlcpy(pr.prompt, p.c_str(), sizeof(pr.prompt));
    }
    _punch.promptSeq += n;

    for(int i = 0; i <= HA_MAX_PLAYERS; i++) {
        _punch.pick[i] = -1;
        _punch.gained[i] = 0;
    }
    _punch.matchIdx = 0;
    _punch.pairRevealing = false;
    _punch.stage = 0; // write
    pt.deadline = now + (uint32_t)PUNCH_WRITE_SECS * 1000;
    pt.phase = 2;
    pushAll();
}

// slot 0 = the prompt where pid is author A of their own pair; slot 1 = the
// prompt where pid is author B of the previous pair. Scanning all pairs for a
// match is O(pairCount) (<= 12) and correct by construction: each pid appears as
// .a in exactly one pair and as .b in exactly one (different) pair.
void punchAnswer(uint8_t pid, int slot, const char* text) {
    if(_active != HA_GAME_PUNCHLINE || _punch.pt.phase != 2 || _punch.stage != 0) return;
    if(_punch.pt.round >= PUNCH_ROUNDS) return; // round 3 uses punchLashAnswer (Task 9)
    for(uint8_t i = 0; i < _punch.pairCount; i++) {
        PunchPair& pr = _punch.pairs[i];
        if(slot == 0 && pr.a == pid && !pr.inA) {
            strlcpy(pr.textA, text, sizeof(pr.textA));
            punchUtf8Truncate(pr.textA, sizeof(pr.textA));
            pr.inA = true;
        } else if(slot == 1 && pr.b == pid && !pr.inB) {
            strlcpy(pr.textB, text, sizeof(pr.textB));
            punchUtf8Truncate(pr.textB, sizeof(pr.textB));
            pr.inB = true;
        }
    }
    if(punchAllWritten()) punchStartVoting(millis());
    else pushAll();
}

bool punchAllWritten() {
    for(uint8_t i = 0; i < _punch.pairCount; i++) {
        PunchPair& pr = _punch.pairs[i];
        if(!pr.inA || !pr.inB) return false;
    }
    return true;
}

void punchStartVoting(uint32_t now) {
    _punch.stage = 1;
    _punch.matchIdx = 0;
    _punch.pairRevealing = false;
    for(int i = 0; i <= HA_MAX_PLAYERS; i++) _punch.pick[i] = -1;
    _punch.pt.deadline = now + (uint32_t)PUNCH_VOTE_SECS * 1000;
    pushAll();
}

// Round advance dispatcher. Rounds 1-2 build a fresh pairing; round 3 hands off to
// Task 9's punchBuildLash(); past round 3, the game is over.
void punchNextRound(uint32_t now) {
    Party& pt = _punch.pt;
    if(pt.round >= PUNCH_ROUNDS) {
        pt.phase = 4;
        pushAll();
        return;
    }
    pt.round++;
    punchBuildPairs(now); // Task 9 branches this to punchBuildLash() on round 3
}

// Time-driven progression: lobby -> countdown -> round transition, plus the
// write-stage deadline. The vote/reveal loop inside stage 1 is Tasks 8-9.
void punchTick(uint32_t now) {
    Party& pt = _punch.pt;
    if(pt.phase == 1) {
        if(partyCountdownDone(pt, now)) {
            pt.round = 0;
            _punch.pack = (uint8_t)punchWinningPack();
            _punch.promptSeq = 0;
            punchNextRound(now);
        }
        return;
    }
    if(pt.phase != 2) return;
    if(pt.round >= PUNCH_ROUNDS) return; // Last Lash tick is Task 9

    if(_punch.stage == 0) {
        // Deadline hit: unwritten slots simply stay "" (inA/inB stay false, which
        // punchRevealPair in Task 8 treats as a zero-vote-eligible empty answer).
        if((int32_t)(now - pt.deadline) >= 0) punchStartVoting(now);
    }
}

// Lobby / countdown / write-stage (with live "done" tracking) / vote-stage
// (placeholder shape -- Task 8 fills in picking/reveal/scoring).
String punchJson(uint8_t pid) {
    Party& pt = _punch.pt;
    if(pt.phase == 0) {
        String s = String("{\"t\":\"punch\",\"phase\":\"lobby\",\"you\":") + pid +
                   ",\"players\":" + partyPlayersJson(pt) + ",\"packs\":[";
        int votes[TRIVIA_MAX_TOPICS] = {0};
        for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
            if(_p[i].used && _punch.vote[i] >= 0 && _punch.vote[i] < _punch.packCount)
                votes[_punch.vote[i]]++;
        for(int i = 0; i < _punch.packCount; i++) {
            if(i) s += ",";
            s += "{\"name\":\"" + ha_json_escape(_punch.packs[i].name.c_str()) +
                 "\",\"votes\":" + votes[i] + "}";
        }
        s += "],\"myvote\":" + String((int)_punch.vote[pid]) + "}";
        return s;
    }
    if(pt.phase == 1)
        return String("{\"t\":\"punch\",\"phase\":\"countdown\",\"sec\":") +
               partyCountdownSec(pt) + "}";
    if(pt.phase == 4)
        return String("{\"t\":\"punch\",\"phase\":\"final\",\"you\":") + pid +
               ",\"scores\":" + playersJson() + "}";

    // phase 2 (play): shape differs by stage.
    if(_punch.stage == 0) {
        String s = String("{\"t\":\"punch\",\"phase\":\"play\",\"stage\":\"write\",\"round\":") +
                   pt.round + ",\"rounds\":" + PUNCH_ROUNDS + ",\"you\":" + pid +
                   ",\"limit\":" + PUNCH_ANSWER_CHARS + ",\"prompts\":[";
        bool first = true;
        for(uint8_t i = 0; i < _punch.pairCount; i++) {
            PunchPair& pr = _punch.pairs[i];
            if(pr.a == pid) {
                if(!first) s += ",";
                first = false;
                s += String("{\"n\":0,\"text\":\"") + ha_json_escape(pr.prompt) +
                     "\",\"done\":" + (pr.inA ? "true" : "false") + "}";
            }
            if(pr.b == pid) {
                if(!first) s += ",";
                first = false;
                s += String("{\"n\":1,\"text\":\"") + ha_json_escape(pr.prompt) +
                     "\",\"done\":" + (pr.inB ? "true" : "false") + "}";
            }
        }
        s += "],\"deadline\":" + String(pt.deadline) + ",\"dur\":" + String(PUNCH_WRITE_SECS) + "}";
        return s;
    }

    // stage 1 (vote): placeholder shape until Task 8 adds picking/reveal/scoring.
    PunchPair& pr = _punch.pairs[_punch.matchIdx];
    bool mine = (pid == pr.a || pid == pr.b);
    String s = String("{\"t\":\"punch\",\"phase\":\"play\",\"stage\":\"vote\",\"round\":") +
               pt.round + ",\"rounds\":" + PUNCH_ROUNDS + ",\"you\":" + pid +
               ",\"match\":" + _punch.matchIdx + ",\"matches\":" + _punch.pairCount +
               ",\"iam\":" + (mine ? "true" : "false") + "}";
    return s;
}
