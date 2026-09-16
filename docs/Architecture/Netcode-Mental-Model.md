# Netcode Mental Model: Desync, Peeker's & Defender's Advantage

A top-down reference for the tradeoffs every networked action game runs into.
This is engine-agnostic theory — the *why* behind lag compensation, the reason
"who shot first" is never a simple question online, and the vocabulary to reason
about it. Read it once and the design choices in any netcode stop looking
arbitrary.

---

## 1. The root cause: there is no single "now"

The whole subject collapses out of one fact: **information takes time to
travel.** By the time a packet about the world reaches you, the world has
already moved on. There is no shared instant that everyone agrees is the
present.

```
   real time ──────────────────────────────────────────────►

   SERVER world:        ●─────●─────●─────●─────●  (the truth, tick by tick)
                              ╲           latency
   what CLIENT sees:           └────────► ●─────●─────●   (the past, delayed)
                                          ▲
                                   you are always looking
                                   at an old snapshot
```

Three clocks are always in play, and they never line up:

- **Server time** — where the authoritative simulation actually is.
- **Client render time** — what a player sees, which is server time minus
  (network latency + a deliberate interpolation buffer).
- **Client input time** — when the player *acts* on what they see, which the
  server won't learn about until another half-trip later.

Every netcode technique is, at bottom, a strategy for managing the gap between
these clocks. You cannot close the gap — physics forbids it — you can only
decide **who pays for it.**

---

## 2. Desync

**Desync** is when two machines disagree about the state of the world in a way
that *matters* — and isn't self-correcting.

It's worth separating two things people both call "desync":

| | Expected divergence | True desync |
|---|---|---|
| **What** | Clients are always slightly behind the server; that's normal. | State has drifted and **stays** wrong. |
| **Cause** | Latency + interpolation delay. By design. | A lost update never resent, a missed event edge, accumulated float error, a determinism break. |
| **Fix** | Nothing — it reconciles continuously. | Something is structurally wrong; it won't heal on its own. |

The dangerous kind is the second. The classic shapes:

- **Dropped-edge desync.** State is sent as "what changed since last tick." If a
  packet carrying a *transition* is lost and the next packet only carries the
  *new* steady state (which hasn't changed since), the receiver never learns the
  transition happened. A door that opened looks shut; an animation state sticks.
  - *Why it happens:* diffing against "last **sent**" instead of "last
    **acknowledged**" state.
  - *Fixes:* resend changed fields for a few extra ticks after a change, or —
    properly — diff against the last *acked* baseline and resend until acked.

- **Determinism drift.** Two machines run "the same" simulation but accumulate
  different floating-point results, or process inputs in a different order, and
  slowly slide apart. Only matters for lockstep/rollback models that *assume*
  identical re-simulation.

- **Authority ambiguity.** Two machines each believe they own an object's truth
  and both write it. Whoever's update lands last "wins," and it flip-flops.

```
   Healthy:   server ●──►  client lags but tracks   ●··· (converging)
   Desync:    server ●      client frozen on stale   ╳   (never closes)
```

The defense against desync is conceptual, not a single feature: **one
authority per fact, reliable delivery of state *transitions*, and reconciliation
that pulls clients back toward truth every tick rather than hoping they stay in
step.**

---

## 3. The core conflict: whose "now" wins

Now the headline tradeoff. In a shooting game, two players interact across the
latency gap, and the engine has to decide whose version of reality is
authoritative *for that interaction.* This is where peeker's and defender's
advantage come from. They are two sides of the same coin — the coin being
**"how does the server treat the time gap between seeing and acting?"**

There are two opposite policies:

### Policy A — Favor the shooter (lag compensation / rewind)

When a shot arrives, the server **rewinds** the world to what the shooter *saw*
at the moment they fired (server time minus their latency), checks the hit
against that rewound state, and applies the result.

> "If it looked like a hit on your screen, it's a hit."

### Policy B — Favor the target (no rewind, present-tense hits)

The server checks every shot against the **current** authoritative world. What
the shooter saw is irrelevant; only where things *actually are now* counts.

> "Only your true present position can be hit."

Almost every modern shooter picks **Policy A**, because Policy B makes hitting a
moving target at any real latency feel impossible — you have to lead by your
ping. But Policy A is exactly what *creates peeker's advantage*. You don't get
to escape the tradeoff; you only get to choose its shape.

---

## 4. Peeker's advantage

**Peeker's advantage**: the player who moves *around a corner* (the peeker) sees
their opponent before that opponent sees them — and under lag compensation, can
shoot them before the defender's screen has even drawn them.

Why it happens, step by step:

```
   t0  Peeker is behind the wall.            Defender sees: empty corner.
   t1  Peeker steps out and sees Defender.   Defender STILL sees: empty corner
       (their own screen is live to them)     (peeker's movement hasn't arrived).
   t2  Peeker fires.                          Defender's screen: peeker just
                                               now appearing — or not yet.
   t3  Shot reaches server. Server REWINDS    Defender had no chance to react;
       to t1 (favor-the-shooter), confirms     they're killed by someone who,
       the peeker saw the defender → HIT.      on their screen, barely existed.
```

The peeker is acting on **fresh, self-authored information** (their own
position, rendered with zero delay). The defender is reacting to **stale,
network-delayed information** (the peeker's position, delayed by the peeker's
upload + the defender's download + interpolation buffer). The defender is
structurally late by the **sum of both players' latencies plus the interpolation
delay** — even with a perfect aim, they were never shown the target in time.

The bigger any of these are, the worse it gets:
- higher latency on either side,
- a longer interpolation buffer (smoother motion, but more delay),
- a higher tick rate *reduces* it (less quantization of the gap) but never
  removes it.

Peeker's advantage is **not a bug** — it's the direct, unavoidable consequence
of favoring the shooter. You traded it for hit-registration that feels fair to
the person aiming.

---

## 5. Defender's advantage

**Defender's advantage** is the mirror image, and it appears when you *don't*
fully favor the shooter — or when the buffers are tuned toward the holder of a
position.

The defender (holding an angle, not moving) has an easier time because:

- They're **stationary**, so their interpolated/delayed position is *accurate* —
  a still target doesn't suffer from being shown a few frames in the past.
- The peeker is **moving**, so the peeker's position is the one being
  extrapolated/interpolated and is therefore the *uncertain* one.
- If the server checks hits in the **present** (Policy B) or applies only
  partial rewind, the defender — who was already aimed at the choke — lands
  shots against a target whose true position the peeker can't fully escape.

```
   Defender (still):  position is certain, pre-aimed, reacting to a known angle.
   Peeker  (moving):  position is delayed/uncertain on the defender's screen,
                       AND on the server if hits are checked present-tense.
```

So the same latency that *helps* a peeker under favor-the-shooter *helps the
defender* under favor-the-target. **The advantage doesn't disappear when you
change the policy — it switches sides.**

---

## 6. The two are one tradeoff

Put them next to each other and the structure is obvious:

| | Server rewinds to shooter's view | Server checks present truth |
|---|---|---|
| **Policy name** | Favor the shooter (lag comp) | Favor the target |
| **Who's advantaged** | The **peeker** (mover) | The **defender** (holder) |
| **Hit-reg feels fair to** | The person aiming | The person being shot at |
| **The complaint you'll hear** | "I got shot behind the wall / before I saw them." | "I hit them dead-on and it didn't register." |
| **Used by** | Most modern shooters | Rare as a pure policy; pieces of it in tac-shooters |

```
                   ◄─── favor target            favor shooter ───►
   defender's advantage  ●──────────────┼──────────────●  peeker's advantage
                                    (your tuning
                                     point lives
                                     somewhere here)
```

You are always somewhere on this axis. Moving toward one end to silence one set
of complaints amplifies the other. The realistic goal is not to eliminate the
advantage but to **place it where it does least harm to your game's feel** and
keep it *consistent* so players can learn it.

---

## 7. The levers

What actually moves you along the axis, and the side effects:

| Lever | Pushes toward | Cost |
|---|---|---|
| **Lag compensation / rewind** | Peeker (shooter) | "Killed behind cover" moments; servers must store position history. |
| **No rewind, present-tense hits** | Defender (target) | Must-lead-your-target; feels broken at high ping. |
| **Interpolation buffer length** | Longer = worse peeker's adv. | Shorter = jittery remote motion. Smoothness vs. fairness. |
| **Tick rate** | Higher = smaller gap both ways | CPU + bandwidth. Reduces, never removes, the effect. |
| **Client-side prediction** | Makes *your own* actions feel instant | Mispredictions cause visible corrections/rubber-banding. |
| **Server-authoritative movement** | Cheat resistance, consistent truth | You *must* predict + reconcile or the local player feels laggy. |
| **Rewind window cap** | Limits how far back you'll favor a shooter | A hard cap on how unfair "behind cover" deaths can get. |
| **Peeker's-advantage mitigations** | Toward defender | e.g. delaying the peeker's *own* view slightly, or "shoot-back time" windows. Controversial; changes feel. |

---

## 8. The mental model in one breath

- There is **no shared now**; every machine sees a different slice of time.
- **Desync** is when that difference stops being temporary and becomes wrong.
  Fix it with single authority, reliable transition delivery, and continuous
  reconciliation — not hope.
- For any contested interaction, you must decide **whose delayed reality is
  authoritative.**
- **Favor the shooter → peeker's advantage.** **Favor the target → defender's
  advantage.** They are the same tradeoff seen from opposite ends.
- You can't delete the advantage, only **relocate** it. Choose where it hurts
  least, keep it consistent, and tune the buffers/tick rate/rewind window to
  taste.

> The art of netcode isn't making the latency gap disappear. It's choosing, on
> purpose and consistently, **who pays for it** — and making that bill feel fair.
