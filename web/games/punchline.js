/* Punchline -- Quiplash-style write-then-vote. The ESP pairs every connected
   player into a ring (you write into your own pair as author A, and into the
   previous pair as author B), so two rounds of paired voting are followed by a
   "Last Lash" round where everyone answers one prompt and spreads 3 votes across
   the field. We send ready/vote(pack)/quip(n,text)/pick(n)/lashvote(target,on)/again. */
(function () {
  var myready = false;
  var writtenSlots = {}; // n -> true, so a submitted textarea doesn't resubmit
  var lastWriteRound = -1; // reset writtenSlots whenever a new round's write stage begins
  var writeCards = {}; // n -> card element, so an in-progress textarea from ANOTHER
  // player's broadcast isn't wiped out mid-round (see renderWrite)

  function sub(name) {
    ["lobby", "count", "write", "vote", "final"].forEach(function (id) {
      $("punch-" + id).classList.toggle("hide", id !== name);
    });
  }

  function renderLobby(m) {
    sub("lobby");
    A.timebarStop("punch-bar"); hide("punch-bar");
    myready = A.readyLobby({ players: m.players, listId: "punch-players", readyId: "punch-ready", meId: "punch-me" });
    // A.readyLobby always overwrites the button's textContent with the current UI
    // language's core/i18n.js "common.ready"/"common.ready_cancel" string (there's no
    // "ru" entry there -- Punchline intentionally doesn't carry that catalog, see the
    // HTML comment above #punch). Put the Russian label back so this screen stays
    // all-Russian regardless of which language the phone's shared chrome is set to.
    $("punch-ready").textContent = myready ? "Готов. Нажми, чтобы отменить" : "Готов";
    A.packVote({
      boxId: "punch-topics",
      packs: m.packs, myvote: m.myvote,
      onVote: function (i) { send({ t: "vote", pack: i }); },
    });
  }

  function renderCount(m) {
    sub("count");
    A.timebarStop("punch-bar"); hide("punch-bar");
    A.countdown("punch-count-num", m.sec);
  }

  function renderWrite(m) {
    sub("write");
    var box = $("punch-prompts");
    // A `punch` message broadcasts on EVERY player's partial submission (up to ~2N
    // times in one write window), not just when something changes for THIS player.
    // Wiping and rebuilding box on every call would blow away every other player's
    // still-open, half-typed textarea (and blur their keyboard on mobile). So we only
    // ever clear+rebuild when a genuinely new round starts; otherwise each prompt slot
    // keeps its own DOM node across renders, tracked in writeCards, and is only ever
    // touched again to swap it to the "sent" view once, the moment it becomes done.
    if (m.round !== lastWriteRound) { writtenSlots = {}; lastWriteRound = m.round; writeCards = {}; box.innerHTML = ""; }
    A.timebar("punch-bar", m.deadline, m.dur, true);
    $("punch-write-meta").textContent = "Раунд " + m.round + " из " + m.rounds;
    var sentHtml = function (p) {
      return '<p class="punch-prompt">' + esc(p.text) + '</p><p class="punch-sent">Отправлено &#10003;</p>';
    };
    var list = m.lash ? [{ n: 0, text: m.prompt, done: m.done }] : m.prompts;
    (list || []).forEach(function (p) {
      var already = writtenSlots[p.n] || p.done;
      var card = writeCards[p.n];
      if (card) {
        // Already rendered this slot. Only touch it if it just became done and
        // hasn't been swapped to the "sent" view yet -- otherwise leave the DOM
        // (including any live typing/focus in its textarea) completely alone.
        if (already && !card.dataset.sent) {
          card.innerHTML = sentHtml(p);
          card.dataset.sent = "1";
        }
        return;
      }
      // First time we've seen this slot this round: create its card.
      card = document.createElement("div");
      card.className = "punch-card";
      writeCards[p.n] = card;
      if (already) {
        card.innerHTML = sentHtml(p);
        card.dataset.sent = "1";
        box.appendChild(card);
        return;
      }
      card.innerHTML =
        '<p class="punch-prompt">' + esc(p.text) + "</p>" +
        '<textarea class="punch-textarea" maxlength="' + m.limit + '" placeholder="Твой ответ..."></textarea>' +
        '<div class="punch-row"><span class="punch-count">' + m.limit + '</span>' +
        '<button class="btn punch-send">Отправить</button></div>';
      box.appendChild(card);
      var ta = card.querySelector(".punch-textarea");
      var cnt = card.querySelector(".punch-count");
      var btn = card.querySelector(".punch-send");
      ta.addEventListener("input", function () { cnt.textContent = String(m.limit - ta.value.length); });
      btn.addEventListener("click", function () {
        var text = ta.value.trim();
        if (!text) return;
        A.sfx("buzz"); A.vibe(15);
        send({ t: "quip", n: p.n, text: text });
        writtenSlots[p.n] = true;
        card.innerHTML = sentHtml(p);
        card.dataset.sent = "1";
      });
    });
  }

  var revealedMatch = -1;
  function renderVote(m) {
    sub("vote");
    A.timebar("punch-vote-bar", m.deadline, m.dur, true);
    var reveal = m.stage === "reveal";
    var box = $("punch-cards");
    box.innerHTML = "";

    if (m.lash) {
      $("punch-vote-meta").textContent = "Последний Смехлыст" + (reveal ? " — Итоги" : " — осталось голосов: " + m.votesLeft);
      $("punch-vote-prompt").textContent = m.prompt;
      (m.answers || []).forEach(function (a) {
        var self = a.pid === A.pid;
        var card = document.createElement("div");
        card.className = "punch-card lash" + (a.mine ? " mine" : "") + (self ? " self" : "");
        card.innerHTML =
          '<span class="punch-nick">' + esc(a.nick) + "</span>" +
          '<p class="punch-ans">' + esc(a.text) + "</p>" +
          (reveal
            ? '<span class="punch-votes">' + a.votes + " голосов · +" + a.gain + "</span>"
            : self ? "" : '<span class="punch-tap">' + (a.mine ? "✓ отдан голос" : "нажми, чтобы проголосовать") + "</span>");
        if (!reveal && !self) {
          card.addEventListener("click", function () {
            var on = !a.mine;
            if (on && m.votesLeft <= 0) return;
            A.sfx("buzz"); A.vibe(12);
            send({ t: "lashvote", target: a.pid, on: on });
          });
        }
        box.appendChild(card);
      });
      return;
    }

    $("punch-vote-meta").textContent = "Раунд " + m.round + " из " + m.rounds + " — пара " + (m.match + 1) + "/" + m.matches;
    $("punch-vote-prompt").textContent = "";
    ["a", "b"].forEach(function (side, i) {
      var text = side === "a" ? m.a : m.b;
      var mine = m.mypick === i;
      var card = document.createElement("div");
      card.className = "punch-card vote-opt" + (mine ? " mine" : "");
      card.setAttribute("role", "button");
      card.innerHTML =
        '<p class="punch-ans">' + esc(text) + "</p>" +
        (reveal
          ? '<span class="punch-votes">' + esc(side === "a" ? m.nickA : m.nickB) + " · " +
            (side === "a" ? m.votesA : m.votesB) + " голосов · +" + (side === "a" ? m.gainA : m.gainB) + "</span>"
          : "");
      if (!reveal && !m.iam && typeof m.mypick === "number" && m.mypick < 0) {
        card.addEventListener("click", function () {
          A.sfx("buzz"); A.vibe(15);
          send({ t: "pick", n: i });
        });
      }
      box.appendChild(card);
    });
    if (reveal && revealedMatch !== m.match) { revealedMatch = m.match; A.sfx("correct"); A.vibe(20); }
    if (!reveal) revealedMatch = -1;
  }

  function renderFinal(m) {
    sub("final");
    A.timebarStop("punch-bar"); hide("punch-bar");
    A.timebarStop("punch-vote-bar"); hide("punch-vote-bar");
    A.podium("punch-podium", m.scores);
    A.sfx("win"); A.vibe([20, 40, 20]);
  }

  A.handlers.punch = function (m) {
    route("punch");
    if (A.view !== "punch") return;
    if (m.phase === "lobby") { writtenSlots = {}; lastWriteRound = -1; writeCards = {}; renderLobby(m); }
    else if (m.phase === "countdown") renderCount(m);
    else if (m.phase === "play" && m.stage === "write") renderWrite(m);
    else if (m.phase === "play") renderVote(m);
    else if (m.phase === "final") renderFinal(m);
  };

  $("punch-ready").addEventListener("click", function () {
    A.sfx("buzz"); A.vibe(15);
    send({ t: "ready", ready: !myready });
  });
  $("punch-again").addEventListener("click", function () {
    A.sfx("start"); A.vibe(20);
    send({ t: "again" });
  });
})();
