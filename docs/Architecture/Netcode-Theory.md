# Netcode: A Complete Theory of Networked Game Simulation

*An academic-level treatment of why real-time multiplayer is hard, what every
technique actually buys you, and the reasoning behind each tradeoff.*

This document is engine-agnostic. It explains the physics and information theory
that constrain all networked games, derives the standard techniques from first
principles, and works through the canonical hard cases — desync, prediction,
reconciliation, lag compensation, peeker's and defender's advantage — in full
detail. It is meant to be read once, start to finish, and leave you able to
reason about any netcode design choice rather than memorize a catalogue of
tricks.

A shorter, diagram-first version of the core tradeoff lives in
`Netcode-Mental-Model.md`. This is the long form: the *why* behind every line of
that one.

---

## Table of contents

0. [How to read this document](#0-how-to-read-this-document)
1. [The fundamental constraint: information has a speed limit](#1-the-fundamental-constraint-information-has-a-speed-limit)
2. [The network is not a wire: what UDP/TCP actually give you](#2-the-network-is-not-a-wire-what-udptcp-actually-give-you)
   *(incl. §2.0 — networking primitives from the ground up: IP, packets, ports, UDP, TCP)*
3. [Time in a distributed system: there is no single "now"](#3-time-in-a-distributed-system-there-is-no-single-now)
4. [Authority: who owns the truth](#4-authority-who-owns-the-truth)
5. [The naïve models and why they fail](#5-the-naïve-models-and-why-they-fail)
6. [Client-side prediction](#6-client-side-prediction)
7. [Server reconciliation](#7-server-reconciliation)
8. [Entity interpolation: rendering other players](#8-entity-interpolation-rendering-other-players)
9. [Extrapolation (dead reckoning) and why it's a last resort](#9-extrapolation-dead-reckoning-and-why-its-a-last-resort)
10. [State synchronization: snapshots, deltas, and baselines](#10-state-synchronization-snapshots-deltas-and-baselines)
11. [Desync: a full taxonomy](#11-desync-a-full-taxonomy)
12. [Lag compensation: rewinding time on the server](#12-lag-compensation-rewinding-time-on-the-server)
13. [The core conflict: whose "now" wins](#13-the-core-conflict-whose-now-wins)
14. [Peeker's advantage, derived](#14-peekers-advantage-derived)
15. [Defender's advantage, derived](#15-defenders-advantage-derived)
16. [The unified tradeoff and the tuning axis](#16-the-unified-tradeoff-and-the-tuning-axis)
17. [The levers and their costs](#17-the-levers-and-their-costs)
18. [Alternative architectures: lockstep and rollback](#18-alternative-architectures-lockstep-and-rollback)
19. [Bandwidth, relevancy, and scale](#19-bandwidth-relevancy-and-scale)
20. [Security: the client is in enemy hands](#20-security-the-client-is-in-enemy-hands)
21. [Measurement: you cannot tune what you cannot see](#21-measurement-you-cannot-tune-what-you-cannot-see)
22. [Worked end-to-end example](#22-worked-end-to-end-example)
23. [Glossary](#23-glossary)
24. [Further reading and lineage](#24-further-reading-and-lineage)

---

## 0. How to read this document

The chapters build on each other. The first four establish the constraints
(speed of light, packet behaviour, distributed time, authority). Chapters 5–10
derive the standard client-server toolkit one technique at a time, each
motivated by a concrete failure of the technique before it. Chapter 11
catalogues how it breaks (desync). Chapters 12–17 are the heart: lag
compensation and the peeker's/defender's-advantage tradeoff, derived rather than
asserted. Chapters 18–21 widen out to alternative architectures, scale,
security, and measurement. Chapter 22 walks a single bullet through the entire
stack.

Throughout, a few conventions:

- **RTT** is round-trip time; **one-way latency** is roughly `RTT/2` but is *not*
  guaranteed symmetric. When the distinction matters the text says so.
- A **tick** is one discrete step of the simulation. Games don't update the world
  continuously; they advance it in small fixed steps — e.g. 30 ticks per second
  ("30 Hz") means the world is recomputed every ~33 ms. "Server tick 100" means
  the 100th such step. **Tick rate** is how many ticks happen per second. Most
  time in this document is measured in ticks.
- Diagrams put **real wall-clock time** on the horizontal axis unless labelled
  otherwise. The single most common mistake in reasoning about netcode is
  conflating "the time a packet describes" with "the time it arrives." The
  diagrams keep them on separate rows on purpose.
- "**The server**" means the authoritative simulation. It can run two ways: as a
  **dedicated server** (a separate machine that only runs the game, no local
  player) or as a **listen-server** (the server runs *inside* one of the players'
  own game clients — that player is both host and participant). The theory is
  identical for both; only the trust and scheduling details differ.
- A **pawn** is the in-world character/avatar a player controls (term borrowed
  from Unreal). "Your pawn" is the thing you drive with your input.

---

## 1. The fundamental constraint: information has a speed limit

Everything in netcode descends from one physical fact: **information cannot
travel faster than light, and in practice travels much slower than that.**

A signal in fibre moves at roughly two-thirds of *c*, about 200,000 km/s. That
sounds instantaneous until you put real distances on it:

```
   distance        one-way (ideal fibre)     realistic RTT (with routing)
   ───────────────────────────────────────────────────────────────────
   same city            < 1 ms                   1 –  10 ms
   500 km               ~2.5 ms                  10 –  30 ms
   2,000 km            ~10 ms                    30 –  60 ms
   transatlantic       ~40 ms                    70 – 110 ms
   transpacific        ~70 ms                   120 – 200 ms
   satellite (GEO¹)    ~120 ms one hop          500 – 700 ms
```

¹ *GEO* = geostationary orbit, ~36,000 km up. The huge round trip to a satellite
that high and back is pure distance, not inefficiency — a stark illustration that
latency is set by the speed of light over the path the signal actually travels.

The "ideal" column is the floor set by physics. The "realistic" column is what
you actually get, because real packets do not travel in a straight line through
vacuum. They:

- pass through **routers** (the devices that relay packets between networks),
  each of which receives the whole packet before forwarding it (*store-and-
  forward*), adding *queueing delay* (waiting in line behind other traffic) and
  *serialization delay* (the time to clock the packet's bits onto the wire one at
  a time);
- traverse **last-mile links** (Wi-Fi, DSL, cable, cellular) that are slower and
  noisier than the backbone;
- sit in **buffers** when a link is congested (this is *bufferbloat*, and it can
  add tens to hundreds of milliseconds under load);
- get **retransmitted** if lost on a reliable channel, which can double or
  triple the effective delay for that piece of data.

The consequence is stark and non-negotiable: **by the time you know something
about a remote player, it is already old.** If your opponent is 60 ms away
(RTT), then the most recent thing you can possibly know about them happened at
least 30 ms ago, and probably 50–80 ms ago once you account for their send rate
and your interpolation buffer.

You cannot engineer this away. No protocol, no amount of bandwidth, no clever
code closes the gap. The entire discipline of netcode is the management of a gap
that is fundamentally **unclosable**. Internalize that now: every later chapter
is a different strategy for deciding *who experiences the gap and how*, never a
strategy for removing it.

> **First principle.** You are never synchronizing the present. You are
> reconciling a set of machines that each hold a *different, delayed* view of a
> shared past, and trying to make their disagreements either invisible or fair.

---

## 2. The network is not a wire: what UDP/TCP actually give you

Games almost universally choose **UDP** *instead of* **TCP** as their network
protocol. (Note: it's UDP *or* TCP — they're alternatives at the same layer, not
one stacked on the other.) Understanding why requires first knowing what these
things actually *are*, and then what each does to your data.

### 2.0 The primitives, from the ground up

If the terms below are already familiar, skip to §2.1. Otherwise, here is the
whole vocabulary the rest of this chapter (and document) leans on, defined
plainly.

**Bit and byte.** A *bit* is a single 0/1. A *byte* is 8 bits. All data on a
network is ultimately a sequence of bytes. "Bandwidth" is measured in bits or
bytes per second; "packet size" in bytes.

**The network protocol stack.** Networking is built in *layers*, each adding
something and handing off to the one below. You only need three for this
document:

```
   ┌─────────────────────────────────────────────────────────┐
   │  YOUR GAME           "player at (10,0,5), hp 100"         │  application
   ├─────────────────────────────────────────────────────────┤
   │  TRANSPORT  (UDP or TCP)   ports, + maybe reliability     │  this chapter
   ├─────────────────────────────────────────────────────────┤
   │  IP (Internet Protocol)    addresses, routing             │  the postal system
   ├─────────────────────────────────────────────────────────┤
   │  physical link  (fibre, Wi-Fi, cellular …)                │  the wires
   └─────────────────────────────────────────────────────────┘
```

Each layer wraps the one above it in its own header (a small prefix of
bookkeeping bytes) and hands the result down. The receiver unwraps them in
reverse. Your game data rides inside all the lower layers' wrapping.

**IP — the Internet Protocol.** The bottom layer this document cares about. IP
is the *postal system* of the internet. Its job is to get a chunk of data from
one machine to another across many intermediate hops, using **IP addresses** to
identify machines (e.g. `192.168.1.7`, or the `127.0.0.1` *loopback* address
that always means "this same machine"). IP makes **no promises**: a piece of
data might arrive, might not, might arrive out of order, might arrive twice. It's
"best effort" — like dropping a postcard in a mailbox with no tracking. Every
higher protocol is built on top of this unreliable foundation.

**Packet / datagram.** A **packet** is one self-contained chunk of data sent
across the network — a header (where it's going, where it's from, etc.) plus a
*payload* (your actual data). "**Datagram**" is the specific word for a packet at
the UDP/IP level; in this document the two are used interchangeably. Packets have
a practical size limit (~1,200–1,500 bytes before they get fragmented), which is
why large messages must be split up. Networking is fundamentally about *packets*,
not a continuous flow — even when a protocol makes it *look* like a continuous
stream, packets are what physically move.

**Port.** An IP address gets data to the right *machine*; a **port** (a number,
0–65535) gets it to the right *program* on that machine. Your game server
"listens on" a port (Fjell defaults to one in its network config); clients send
to `address:port`. Think of the IP address as a building's street address and
the port as the apartment number. Ports are the one thing UDP adds over raw IP
besides an error-detecting checksum.

**Latency, RTT, jitter, loss, bandwidth.** Five properties of a link, used
constantly below and defined fully in §2.4: how *delayed* data is (latency), how
long a *round trip* takes (RTT), how much that delay *varies* (jitter), how often
packets *vanish* (loss), and how much data per second *fits* (bandwidth).

With those in hand, UDP and TCP are easy to state:

- **UDP — User Datagram Protocol.** A razor-thin layer over IP. It adds *ports*
  (so data reaches the right program) and a *checksum* (so corrupted data is
  detected and dropped) — and **nothing else.** It inherits all of IP's
  non-promises: datagrams may be lost, reordered, or duplicated, and UDP won't
  tell you or fix it. It is "fire and forget." That sounds useless, but it's
  exactly the blank slate a game wants (§2.3).

- **TCP — Transmission Control Protocol.** A thick layer over IP that turns the
  unreliable packet postal system into a **reliable, ordered byte stream**: it
  detects lost packets and *resends* them, *reorders* what arrives out of order,
  and hides packet boundaries so it looks like one continuous pipe. It also slows
  itself down when the network is congested. This is perfect for web pages and
  file transfers — and, as §2.2 shows, wrong for real-time game state.

The rest of the chapter is about *which* of these to build on and what you must
add back yourself.

### 2.1 The properties a transport can offer

There are four largely independent properties a transport might give a stream of
messages:

1. **Reliability** — lost messages are detected and resent until they arrive.
2. **Ordering** — messages are delivered in the order they were sent.
3. **Framing** — message boundaries are preserved (you receive whole messages,
   not a byte stream you must re-frame yourself).
4. **Congestion/flow control** — the sender slows down to avoid overwhelming the
   link or the receiver.

TCP bundles **all four** together and does not let you opt out of any. UDP gives
you **none** of them: it is a thin wrapper over IP that adds only ports and a
checksum. You get datagrams that may arrive, may not, may arrive out of order,
and may even arrive twice.

### 2.2 Why TCP is the wrong default for action games

TCP's reliability and ordering are *coupled*, and that coupling is fatal for
real-time state. The killer is **head-of-line blocking**:

```
   sent:     [A][B][C][D][E]          (positions at ticks 1..5)
   lost:         ✗  (B never arrives)
   TCP holds:    [A] ……… waiting ………  C, D, E are RECEIVED but
                                         NOT DELIVERED until B is resent
   you wanted:   [A]    [C][D][E]      — B is stale anyway; show the newest!
```

Because TCP guarantees in-order delivery, a single lost packet stalls *every
later packet* until the loss is repaired by a retransmission — which takes at
least one more RTT. For a position stream this is exactly backwards: the data
behind the lost packet is *newer and more valuable*, and the lost packet itself
is usually worthless by the time it could be resent. TCP will faithfully deliver
you a stale position you no longer care about, while holding hostage the fresh
ones you do.

TCP's congestion control compounds the problem: it interprets loss as
congestion and throttles the whole connection, introducing latency spikes
exactly when the network is already struggling.

### 2.3 Why UDP, and what you must rebuild on top of it

UDP's "give me nothing" is a feature: it lets the game **choose per-message**
which guarantees it wants. The standard pattern, which Fjell follows, is two
logical channels:

- **Unreliable, latest-wins** — for continuous state (positions, rotations,
  animation parameters). If a packet drops, you don't resend it; the next one
  supersedes it anyway. Newer always beats older. No head-of-line blocking.
- **Reliable, ordered** — for *events* and *transitions* that must not be lost:
  *spawns* and *despawns* (objects being created and destroyed in the world),
  *RPCs* (remote procedure calls — making a function run on another machine, e.g.
  "play this sound on all clients"), and the connection *handshake* (the initial
  back-and-forth that establishes a session and assigns the new client its
  identity). These are rare and discrete, so the cost of reliability is
  acceptable, and losing one genuinely corrupts state.

This split is the single most important transport decision in the engine, and it
recurs everywhere. The rule of thumb:

> **Replicate continuous state unreliably; replicate state *transitions*
> reliably.** A position is continuous — miss one, the next fixes it. "The door
> opened" is a transition — miss it and the receiver is permanently wrong until
> something else happens to correct it. (Chapter 11 shows exactly how this turns
> into a desync bug when violated.)

Libraries like ENet (what Fjell uses), as well as the transport layers inside
GameNetworkingSockets, RakNet, and similar, exist precisely to give you reliable
*and* unreliable channels over UDP, plus fragmentation of large messages,
sequencing, and connection management — without dragging in TCP's head-of-line
blocking across the whole connection.

### 2.4 The properties you must measure

Because UDP guarantees nothing, the game must observe the link itself:

- **Latency** — the one-way delay for data to travel from sender to receiver,
  measured in milliseconds (ms). It's literally "how far in the past" a piece of
  remote data is by the time you see it.
- **RTT (round-trip time)** — the time for a packet to go *to* the other machine
  *and* a reply to come back. It's what you can actually measure directly (send a
  timestamped ping, see when the echo returns); one-way latency is roughly `RTT/2`
  but is *not* guaranteed to be symmetric (the two directions can differ).
- **Jitter** — the *variance* in latency. Jitter is often nastier than raw
  latency: a steady 80 ms is easy to hide, but 40 ms swinging to 120 ms and back
  defeats fixed-size buffers and causes visible hitches. Your interpolation
  buffer must be sized for jitter, not just mean latency.
- **Packet loss** — the fraction of datagrams that never arrive. Loss interacts
  viciously with delta compression (Chapter 11).
- **Bandwidth** — bytes per second, in each direction, which caps how much state
  you can ship how often (Chapter 19).

A good netcode harness exposes all four live, *and* can synthesize them
artificially (a *conditioning simulator*) so you can reproduce a 150 ms / 5 %-loss
link on a **LAN** (Local Area Network — machines on the same local network, such
as two computers on the same home Wi-Fi/router, where latency and loss are
naturally near zero). You cannot tune what you cannot see or reproduce;
Chapter 21 returns to this.

---

## 3. Time in a distributed system: there is no single "now"

This chapter is the conceptual core of the whole document. Everything painful
about netcode is a consequence of the fact that **"now" is not a shared
quantity.**

### 3.1 Three clocks, never aligned

In a client-server game, at any wall-clock instant, at least three different
"current times" are in play *for a single client*:

```
   real wall-clock time ──────────────────────────────────────────────►

   (1) SERVER simulation time
       the authoritative tick the server is computing right now
       ●────●────●────●────●────●────●────●────●   tick 100,101,...

   (2) CLIENT render time   = server time − (latency + interp buffer)
       what this client is DRAWING right now; deliberately in the past
                       ●────●────●────●────●         showing ~tick 96
                       └─ behind by latency + buffer

   (3) CLIENT input time
       when the player physically acts on what they see at render time;
       the server won't learn of it until ~half an RTT later
                                          ▲
                                    player clicks here, on a frame
                                    that depicts server-tick ~96,
                                    server receives it around tick ~103
```

- **Server time (1)** is the only authoritative clock. The world *is* whatever
  the server computed.
- **Render time (2)** is where the client's *display* lives. It is intentionally
  set in the past, behind server time by network latency plus a deliberate
  buffer (Chapter 8), so that the client always has at least two real samples to
  interpolate between and never has to guess.
- **Input time (3)** is the cruelest. The player reacts to what they *see*, which
  is render time — already old. Their reaction then takes another ~half-RTT to
  reach the server. So an input is, from the server's perspective, a decision
  made about a world state that is now *latency + buffer + half-RTT* in the past.

The total "age" of the information a player acts on is therefore on the order of:

```
   action lag ≈ (server→client latency)        ← how stale your view is
              + (interpolation buffer)          ← deliberate smoothing delay
              + (client→server latency)         ← how long your input takes to land
```

For a 60 ms-RTT player with a 2-tick (~66 ms at 30 Hz) buffer, that's roughly
`30 + 66 + 30 ≈ 126 ms` between "the world was actually like this" and "the
server acts on my response to it." Every technique in Chapters 6–17 is a way of
hiding, shifting, or apportioning that ~126 ms.

### 3.2 Clock synchronization and the server timeline

To interpolate and to do lag compensation, clients need a shared notion of
server time — not the wall clock, but the server's *tick timeline*. This is
established by a lightweight clock-sync handshake:

1. The client periodically pings the server with its local send timestamp.
2. The server echoes back, including its current tick/time.
3. The client measures RTT from the round trip and estimates the offset between
   its clock and the server's, discarding outliers (the jitter again) and
   smoothing the estimate.

The result is an estimate of "what server tick is happening *right now*", which
the client deliberately runs *behind* (for rendering remotes) and *ahead* (for
sending inputs the server should apply at a future tick — see Chapter 18 on
input delay). The estimate is never perfect; it drifts with jitter and must be
continuously nudged, not set once.

> **Key idea.** A client does not have "the time." It has an *estimate of the
> server's timeline* plus two deliberate offsets: it renders the past and (in
> some designs) sends toward the future. Confusing these offsets is the source
> of an enormous class of bugs.

### 3.3 Why you can't just "send faster"

A natural instinct is: if the problem is staleness, increase the tick rate and
the send rate. This helps — at 128 Hz the world is sampled every 7.8 ms instead
of every 33 ms at 30 Hz, so the quantization component of the gap shrinks. But:

- It does **nothing** to the latency component, which is set by physics and
  routing (Chapter 1).
- It multiplies bandwidth and CPU cost linearly (Chapter 19).
- It has diminishing returns: once your sampling interval is well below your
  jitter, finer sampling stops mattering because jitter dominates.

So tick rate is a real lever (Chapter 17), but a bounded one. It reduces the gap;
it never removes it. The unclosable part is the speed of light.

---

## 4. Authority: who owns the truth

Before we can discuss *synchronizing* state, we must decide *whose* state is
canonical. This is **authority**, and getting it conceptually clean prevents an
entire family of bugs.

### 4.1 The authoritative-server model

The dominant model for competitive and cooperative action games is the
**authoritative server**: exactly one machine — the server — owns the true state
of the world. Clients hold *copies*, send *requests* (inputs), and *display*
what the server tells them. When a client and the server disagree, **the server
is right, always.** The client corrects itself toward the server (Chapter 7).

The reason is twofold:

1. **Consistency.** With a single writer per fact, there is never an ambiguous
   "whose value wins" — the authority's value wins. (Chapter 11 shows what
   happens without this.)
2. **Security.** The client runs on a machine the player controls and can modify.
   Anything the client is *trusted* to decide can be cheated. Only state the
   server validates is safe (Chapter 20).

### 4.2 Authority vs. possession — a distinction worth its own section

There are two different questions that beginners constantly merge:

| Question | Name | Who answers "yes" |
|---|---|---|
| "May I write this object's *true* state?" | **Authority** | The server, for **every** object. A client, only for objects explicitly delegated to it. |
| "Does the local *player* drive this object?" (camera target, input receiver) | **Possession / ownership** | Whoever the object's `owner` is — the local player only for *their* pawn. |

These come apart constantly:

- The **listen-server host** has authority over the *entire world* but only
  *possesses* the one pawn it plays. It should run input/camera logic only for
  its own pawn (possession), but it may write replicated state for everything
  (authority).
- A **client** possesses its own pawn (drives its camera and input from it) but
  has authority over *nothing it does not own* — even its own pawn's true
  position is the server's to finalize.

Gating the wrong thing on the wrong predicate is a classic bug. Camera and input
must key off **possession** ("is this *mine* to drive?"). Writing replicated
state must key off **authority** ("am I allowed to set the *truth* here?").
Using "do I have authority" where you meant "do I own this" makes the host drive
every pawn's camera; using "do I own this" where you meant "do I have authority"
lets clients write state they shouldn't.

> Name them differently in code and never alias them. (Fjell uses
> `is_local_authority` vs `is_owned_by_local` for exactly this reason; the
> distinction is documented as a hard rule.)

### 4.3 Delegated / distributed authority

Some designs delegate authority for *specific* objects to specific clients —
e.g. a client is authoritative over its own pawn's movement to make movement
feel perfectly responsive, while the server remains authoritative over
everything that *matters competitively* (health, hits, pickups). This is a
spectrum:

- **Fully server-authoritative movement** — most cheat-resistant; requires
  prediction + reconciliation (Chapters 6–7) or the local player feels laggy.
- **Client-authoritative movement** — simplest, feels great locally, but trusts
  the client about its own position, which is exploitable ("speedhack",
  teleport). Acceptable for co-op and casual; dangerous for competitive.

The choice is a security/feel tradeoff, and it is independent of the
interpolation/lag-comp choices later. A common shippable middle ground:
client-predicted movement that the server *validates* (rejects physically
impossible moves) rather than blindly trusts.

---

## 5. The naïve models and why they fail

To motivate the real techniques, watch the obvious approaches break. Each
failure points directly at the next technique.

### 5.1 Naïve model A: "dumb client", input goes to server, server sends back state

The simplest authoritative design:

1. Client samples input, sends it to the server.
2. Server applies input, advances the simulation, sends the new state back.
3. Client displays whatever it most recently received.

This is perfectly *correct* and perfectly *unplayable* at any real latency. The
problem is **input lag on your own character**:

```
   t=0    player presses "forward"
   t=0    input leaves client                      ─┐
   t≈RTT/2 server receives, moves the pawn          │  the player's own
   t≈RTT  client receives the new position          │  avatar does not move
          and finally draws the pawn one step ahead ─┘  until a full RTT later
```

At 100 ms RTT, your character responds 100 ms after you press the key, *every
time*. This feels like wading through treacle and is universally rejected.
Quake-era players called the fix for this "client-side prediction." → Chapter 6.

### 5.2 Naïve model B: "trust the client", each client simulates and broadcasts its own state

Swing the other way: let each client simulate its own player fully and just
broadcast the result.

- **Feel:** perfect — zero input lag locally.
- **Consistency:** broken — there's no single authority, so two clients can
  disagree about a shared object and there's no principled resolution
  (Chapter 11's authority ambiguity).
- **Security:** catastrophic — every client is trusted about its own state, so
  every client can lie. Position, health, hit results — all forgeable
  (Chapter 20).

This is why "trust the client" survives only in cooperative or non-competitive
contexts. The lesson: **local responsiveness and global authority are in
tension, and the resolution is to *predict locally but defer to the server*** —
which is precisely prediction + reconciliation.

### 5.3 Naïve model C: render remote players exactly as their packets arrive

For *other* players, just move them to whatever position each packet reports, the
instant it arrives.

- Packets arrive every ~33 ms (30 Hz) but the game renders at 144 Hz, so remote
  players **teleport** between sampled positions and **freeze** between packets.
- Jitter makes the arrival times irregular, so the motion is not even evenly
  stuttery — it's lurching.
- A single dropped packet makes them freeze for an extra interval, then snap.

The fix is to deliberately render remotes slightly in the past and *interpolate*
between two known real samples, trading a little more latency for smooth motion.
→ Chapter 8.

These three failures map cleanly onto the three pillars of the standard model:
**predict your own character, reconcile against the server, interpolate everyone
else.**

---

## 6. Client-side prediction

**Client-side prediction** removes the input lag of naïve model A by having the
client *immediately simulate the effect of its own input* without waiting for
the server to confirm it.

### 6.1 The mechanism

```
   client                                            server
   ──────                                            ──────
   sample input #42 ──────────────────────────────►  (in flight, ~RTT/2)
   IMMEDIATELY apply #42 locally:                     
     move my pawn forward this tick                   
     (predicted state, not yet confirmed)             
   keep a copy of input #42 in a pending buffer       
                                                       receives #42,
                                                       applies it authoritatively,
   ◄─────────────────────────────────  sends back state + "last input I applied: 42"
   compare server's pawn position to MY predicted
   position at the time I sent #42 …  (→ Chapter 7)
```

The client predicts using the *same simulation code* the server runs (this code
sharing is essential — divergent logic guarantees mispredictions). It applies
input the instant it's sampled, so the local pawn responds with **zero
perceptible lag**.

### 6.2 The pending-input buffer

The client cannot throw away an input the moment it predicts with it, because the
server hasn't confirmed it yet. It stores every unacknowledged input in a
**pending buffer**, tagged with a sequence number and the tick it was applied at.
When the server acknowledges "I have applied up to input #42", the client can
discard #42 and earlier. The pending buffer is what makes reconciliation
possible (next chapter): it's the record of "everything I did that the server
hasn't confirmed yet."

### 6.3 What can and cannot be predicted

Prediction works for anything whose outcome the client can compute the same way
the server will:

- **Movement** — the headline case. Deterministic given input + current state.
- **Firing animations, ability activations** — local cosmetic response can be
  predicted even if the *result* (did it hit?) is server-decided.
- **Pickups, doors, anything the client can deterministically resolve.**

Prediction *cannot* reliably cover things that depend on information the client
doesn't have or shouldn't be trusted with:

- The result of a contested interaction (did my bullet hit *that other player*,
  whose true position only the server knows authoritatively).
- Anything involving another player's hidden state, or random numbers whose
  *seed* (the starting value that determines the whole sequence — see §11.3) the
  client doesn't share.

Predicting these anyway leads to frequent, visible mispredictions ("I saw the
hit marker but they didn't die"). The art is predicting the *responsive* parts
(your own motion and animations) while leaving *authoritative outcomes* to the
server, and designing the feedback so a corrected misprediction is graceful.

---

## 7. Server reconciliation

Prediction is a *guess*. Sometimes the guess is wrong — the server applied a
collision the client didn't foresee, or another player's action altered the
shared world. **Reconciliation** is how the client corrects its prediction to
match the authoritative server *without throwing away the inputs that have
happened since*.

### 7.1 The naïve correction and why it's wrong

The obvious fix — "when the server's state disagrees, snap my pawn to the
server's value" — produces brutal rubber-banding, because the server's value is
*old*. The server is telling you where you were `RTT/2 + buffer` ago. If you
snap to it, you erase all the movement you've correctly predicted since then,
then re-predict it next frame, then get corrected again. The pawn jitters
backward every correction.

### 7.2 Reconciliation done right: replay the pending inputs

The correct algorithm uses the pending-input buffer from Chapter 6:

```
   server says: "after applying input #42, your pawn was at position P."
   client:
     1. set pawn to P                      (accept the authoritative past)
     2. discard inputs ≤ #42 from pending   (they're now confirmed)
     3. RE-SIMULATE inputs #43, #44, #45…   (everything sent since #42)
        applying each to P in order, with the same sim code
     4. the result is the corrected PRESENT prediction
```

The client trusts the server about the *past* (position after input #42) but
*re-derives the present* by replaying the inputs it knows happened since. If the
client's prediction was already correct, the replay lands in exactly the same
place and the player sees nothing. If it was wrong, only the *delta* between the
mispredicted and corrected present is visible, and it's usually tiny.

### 7.3 Smoothing corrections

Even a correct replay can produce a small positional pop when a misprediction is
resolved. Production systems **smooth** the correction: rather than snapping the
visual pawn to the corrected position, they move the *render* position toward the
corrected *simulation* position over a few frames (an error offset that decays to
zero). The simulation is authoritative immediately; the *rendering* eases in. The
threshold matters:

- **Small error** → ease it out over a handful of frames (invisible).
- **Large error** (teleport, big collision, long stall) → snap immediately;
  easing a huge error just produces a slow, wrong slide.

This is the subtlest, most bug-prone code in any netcode, because it runs every
frame on the object the player is staring at, and any asymmetry between
prediction and server simulation shows up here as persistent jitter.

> **Why prediction + reconciliation is the canonical answer.** It gives the local
> player *zero-lag response* (prediction) while keeping the server *fully
> authoritative* (reconciliation corrects every divergence). It resolves the
> tension Chapter 5 exposed — responsiveness vs. authority — instead of picking a
> side.

---

## 8. Entity interpolation: rendering other players

Prediction and reconciliation handle *your own* pawn. **Other** players cannot be
predicted — you don't have their inputs. The technique for them is **entity
interpolation**, the fix for naïve model C.

### 8.1 Render in the past, on purpose

The client deliberately renders remote entities at a time **behind** the newest
data it has received — typically one to two send-intervals' worth (the
*interpolation buffer* or *interp delay*). Because it's rendering the past, it
always has at least two real snapshots bracketing the render time, and can
**interpolate** between them:

```
   snapshots received (server ticks):   ●96      ●97      ●98   (newest)
   real time the client has them by:    arrive irregularly (jitter)

   client render time is set to:               ▲ here (between 96 and 97)
                                                └─ ~2 ticks behind newest

   rendered position = lerp(snap96, snap97, fraction)
                       ("lerp" = linear interpolation: blend two values by a
                        0..1 fraction; fraction 0.25 = a quarter of the way
                        from snap96 toward snap97)
                       — always between two KNOWN samples, never a guess
```

The buffer must be at least one send interval (so a single missed/late packet
still leaves you with a bracketing pair), and is usually sized to absorb your
expected jitter. Bigger buffer → smoother motion and more loss-tolerance, but
more latency on remote players (you see them further in the past — this directly
feeds peeker's advantage, Chapter 14). Smaller buffer → fresher remotes but
jitter and freezes when packets are late. This is a real, tunable tradeoff, not a
"correct value."

### 8.2 What interpolation costs and why it's still worth it

Interpolation *adds* latency to your view of other players — you're choosing to
see them ~66 ms in the past to get smooth motion. This seems perverse given
Chapter 1's whole point is that latency is the enemy. But the alternative
(naïve model C) is unplayably stuttery, and the added delay is small and
*constant*, which the human visual system tolerates far better than jitter.
Constant lateness can be aimed-through and learned; jitter cannot.

### 8.3 Interpolation vs. the local pawn — the asymmetry

Note the deliberate asymmetry that confuses newcomers:

| Object | Technique | Time it's rendered at |
|---|---|---|
| **Your own pawn** | Prediction + reconciliation | The *present* (predicted) — zero lag |
| **Remote pawns** | Interpolation | The *past* (buffered) — smooth but delayed |

You see *yourself* in the present and *everyone else* in the past. This asymmetry
is not a bug — it's the whole point. It's also the seed of peeker's advantage:
when two players peek each other, each sees *themselves* live and the *other* one
delayed.

---

## 9. Extrapolation (dead reckoning) and why it's a last resort

Interpolation needs two bracketing samples. When the newer one hasn't arrived
(packet late or lost), the client has only the past — to keep moving the entity
it must **extrapolate**: predict the remote's future from its last known velocity
(also called *dead reckoning*).

```
   have:   ●96 ──► ●97   (then nothing; 98 is late)
   render time advances past 97 →
   extrapolate:    ●97 ··········►◌  guess using last velocity
                                     (no real sample backing this)
```

Extrapolation is a **guess about another player's future**, and players are not
ballistic — they change direction unpredictably. So extrapolation is:

- **Acceptable for very short gaps** (one missed packet) and for
  near-ballistic objects (projectiles, vehicles with momentum).
- **Dangerous for agile entities** over longer gaps: you'll confidently render a
  player running straight while they actually juked, then *snap* them sideways
  when the real packet finally lands. The snap is more jarring than a brief
  freeze would have been.

Most systems extrapolate only a bounded amount (e.g. up to ~100–250 ms) and then
freeze rather than fantasize further. The choice between "freeze" and
"extrapolate then snap" under loss is a feel decision; neither is free.

> Interpolation is **between two truths**. Extrapolation is **beyond the last
> truth**. The first is safe; the second is a bet. Prefer the first; bound the
> second.

---

## 10. State synchronization: snapshots, deltas, and baselines

We've covered *how* a client uses received state. Now: how the server *sends* it
efficiently, and the bandwidth/robustness tradeoffs hidden in that choice. This
chapter sets up the desync taxonomy in Chapter 11.

### 10.1 Full snapshots

The simplest scheme: every tick, send the complete state of every relevant
object. Robust (a lost packet is fully replaced by the next) but expensive —
bandwidth scales with `objects × fields × tickrate`, most of it re-sending values
that didn't change.

### 10.2 Delta compression against the last sent state

To cut bandwidth, send only what *changed*. The server keeps a **shadow copy** —
the last state it sent for each object — and each tick diffs the live state
against the shadow, transmitting only the differing fields:

```
   live this tick:   pos=(10,0,5)  hp=100  anim=run
   shadow (last sent):pos=(9,0,5)   hp=100  anim=run
   diff → send only:  pos changed → (10,0,5)
                       hp,anim unchanged → omit
   then: shadow ← live
```

This is hugely effective — idle objects cost nothing, and a fast scene mostly
ships a few moving transforms. Fjell's `ReplicationGraph` works exactly this
way: diff each `[[fj::replicated]]` field + transform against a stored shadow,
ship only changes, on the unreliable channel.

But delta compression introduces a **dependency**: a delta is only meaningful
relative to the baseline the receiver actually has. And on an unreliable channel,
packets drop. This is the crack that desync grows in (Chapter 11).

### 10.3 The baseline problem and "diff against last *acked*"

The subtle question is: **what should the server diff against?**

- **Diff against last *sent*** — cheap and obvious, but *wrong* under loss. If the
  packet carrying a change is lost, the server has already updated its shadow to
  the new value, so next tick it sees "no change" and sends nothing. The receiver
  *never learns about the change*. → Chapter 11's dropped-edge desync.
- **Diff against last *acknowledged*** — correct, but requires the receiver to ack
  what it actually got, and the server to keep per-peer history of unacked
  baselines. The server keeps re-sending the change until the client confirms it.
  More state, more bookkeeping, but no silent loss.

Quake 3's snapshot system and most production engines use the *acked-baseline*
approach for exactly this reason. The cheap "diff against last sent" is a common
early implementation that works *until you add real packet loss*, at which point
it produces intermittent, maddening state bugs.

### 10.4 Reliable transitions vs. unreliable state, restated

This is where Chapter 2's channel rule earns its keep. Two correct designs:

1. **Continuous state unreliable + diff against acked baseline.** Loss is fine;
   the next delta (still relative to a correct baseline) repairs it.
2. **State transitions reliable.** Discrete edges ("door opened", "weapon
   switched", "spawned") go on the reliable channel so they cannot be silently
   dropped.

The bug is mixing them wrong: putting a *transition* on an unreliable channel
*and* diffing against last-sent. Then a dropped packet eats the edge forever.

---

## 11. Desync: a full taxonomy

**Desync** is when two machines disagree about the world in a way that *matters
and does not self-correct*. This chapter classifies every common form, its root
cause, and its fix. It's the diagnostic chapter — when something is wrong on the
network, this is the checklist.

### 11.1 Expected divergence is not desync

First, separate the benign from the pathological:

| | Expected divergence | True desync |
|---|---|---|
| **What** | Clients are always slightly behind the server; remotes are rendered in the past. | State has drifted and *stays* wrong. |
| **Cause** | Latency + interpolation buffer. By design (Chapters 3, 8). | A lost edge never repaired, accumulated error, a determinism break, ambiguous authority. |
| **Self-heals?** | Yes — continuously, every snapshot. | No — structurally broken; will not fix itself. |
| **Action** | None; it's the system working. | Find the structural cause below. |

The mistake of treating expected divergence as a bug leads people to "fix" it by
shrinking buffers to zero and reintroducing jitter, or by over-trusting clients.
The mistake of treating true desync as expected divergence leads to shipping a
game that quietly drifts wrong. Tell them apart first.

### 11.2 Dropped-edge desync

The most common true desync, and the one Chapter 10.3 set up.

```
   server door state:  closed ──► OPEN ───────────────────────────
   tick:                 t1     t2        t3        t4
   sends:               (closed)(OPEN)*   (no chg)  (no chg)
                                  └─ * THIS PACKET IS LOST
   client receives:     closed   —        —         —
   client believes:     closed   closed   closed    closed   ← permanently wrong
```

- **Root cause:** a state *transition* on an unreliable channel, diffed against
  *last sent*. Once the shadow advances past the edge, the edge is gone.
- **Symptom:** doors that look shut but aren't, animation states stuck, a flag
  that never flips, health that "teleports" the next time it changes.
- **Fixes (any one):** put the transition on the **reliable** channel; or diff
  against the **acked** baseline and keep resending until acked; or, as a weaker
  band-aid, redundantly resend changed fields for a few extra ticks (raises the
  odds the edge survives, but doesn't guarantee it).

### 11.3 Determinism drift (lockstep/rollback only)

In architectures that assume every machine re-simulates *identically* (lockstep,
rollback — Chapter 18), the machines exchange only inputs and each computes the
same world. This only works if the simulation is **deterministic** — meaning the
same inputs *always* produce the exact same outputs, bit for bit, on every
machine. If their computations diverge even slightly, they slide apart and
*never reconverge*, because nothing exchanges authoritative state to pull them
back.

Two pieces of jargon below: an **RNG** is a *random number generator*, and its
output is actually determined by a starting **seed** value — same seed, same
sequence of "random" numbers. **Fixed-point math** represents fractional numbers
using integers with an implied decimal point; unlike *floating-point* (the usual
way computers store fractions), fixed-point gives bit-identical results across
different CPUs and compilers, which is exactly what determinism needs.

- **Root causes:** floating-point results differing across CPUs/compilers/flags;
  iterating an unordered container (a collection with no guaranteed order) in a
  different order on different machines; using uninitialized memory or
  address-dependent values; consuming a shared RNG in a different order (so two
  machines pull different "random" numbers); non-deterministic physics solvers.
- **Symptom:** the simulations agree for a while, then visibly fork; in
  competitive lockstep this is a "desync" that drops a player or aborts the
  match.
- **Fixes:** enforce determinism rigorously — fixed-point or carefully constrained
  floating-point math, deterministic iteration order, a single seeded RNG
  consumed in lockstep, a fixed timestep (Chapter 18.4), and periodic *checksums*
  of the world state so a divergence is *detected* the moment it happens rather
  than discovered minutes later.

This entire class **does not exist** in a pure authoritative-server +
state-replication model, because the server continuously ships authoritative
state and clients are always being pulled back toward it. That robustness is one
of the strongest arguments for the client-server model over lockstep for games
that don't strictly need lockstep's bandwidth profile.

### 11.4 Authority ambiguity

Two machines both believe they own a fact and both write it.

```
   client A writes object X: pos = (10, …)
   client B writes object X: pos = (4, …)
   server / other clients see whichever update landed last → flip-flop
```

- **Root cause:** no single authority per fact (naïve model B), or a delegated
  ownership that wasn't cleanly transferred (two owners during a handoff).
- **Symptom:** an object that jitters between two values, "fighting" updates,
  ownership that ping-pongs.
- **Fix:** exactly one authority per fact at any instant. Ownership transfer must
  be a clean handoff — old owner stops writing *before* new owner starts, usually
  mediated by the server. (This is why Chapter 4's authority/possession
  cleanliness matters in practice.)

### 11.5 Float and integration error accumulation

Even with correct delivery, two simulations *integrating* motion with slightly
different timesteps or orders accumulate small errors that grow without bound if
nothing corrects them. (*Integration* here is the physics sense: repeatedly
advancing a value by small steps — adding velocity×dt to position each tick to
move an object. Tiny per-step differences pile up over many ticks.) In a server-authoritative model this is mostly a
non-issue (the server's value is shipped and wins), but it bites client
prediction (Chapter 7): if the client's integration differs from the server's,
every tick adds a little reconciliation error, producing perpetual low-level
jitter on the local pawn.

- **Fix:** share the exact simulation code and timestep between client prediction
  and server; reconcile against authoritative state; use a fixed timestep so the
  integration is identical (Chapter 18.4).

### 11.6 Ordering and duplication hazards

UDP can deliver out of order or duplicate datagrams. Applying an *older* state
update after a newer one rolls the object backward; applying a duplicated event
twice double-counts it.

- **Fix:** *sequence numbers* on state (a counter stamped on each packet, so the
  receiver can say "apply only if newer than what I already have"; latest-wins for
  continuous state) and *idempotency* or *dedup* on events. Idempotency means
  applying the same event twice has the same effect as applying it once; dedup
  (deduplication) means detecting and discarding a repeat. Either way, a spawn
  that arrives twice must not create two objects.

### 11.7 The unifying defense

Every true desync above is defeated by the same three principles, which is why
they're worth stating as invariants:

> 1. **One authority per fact.** No ambiguity about whose value is true.
> 2. **Reliable delivery of *transitions*; correct baselines for *state*.** Edges
>    can't be silently dropped; deltas are diffed against what the receiver
>    actually has.
> 3. **Continuous reconciliation, not hope.** Clients are pulled toward truth
>    every tick; you never *assume* two machines stay in step — you *make* them.

If a desync survives all three, it's almost always a determinism break
(Chapter 11.3) hiding in code that *assumed* re-simulation when it should have
replicated state.

---

## 12. Lag compensation: rewinding time on the server

Now we reach the techniques that make *combat* work, and the ones that create the
peeker's/defender's tradeoff. The problem: a player aims at what they *see*,
which (Chapter 3) is the past. If the server checks their shot against the
*present*, the shot misses anything that moved — at 100 ms RTT you'd have to lead
every target by its velocity times your ping. That's unplayable. **Lag
compensation** fixes it by letting the server evaluate the shot against the world
*as the shooter saw it*.

### 12.1 The mechanism: server-side rewind

The server keeps a short **history buffer** of every player's position (and
*hitbox* pose — the hitbox being the simplified collision shape, e.g. a capsule
or set of boxes, that shots are actually tested against instead of the detailed
visual mesh) for the last several hundred milliseconds. When a shot arrives:

```
   1. shot packet carries (or the server infers) the client's render time:
      "I fired while seeing the world as it was at server-time T_see"
      where T_see = server_now − (shooter's latency + their interp buffer)

   2. server REWINDS every relevant entity to its position at T_see
      (looked up / interpolated from the history buffer)

   3. server tests the shot RAY against that REWOUND world
      (a "ray" = a line starting at the gun muzzle going along the aim
       direction; "did it hit?" means "does that line cross a hitbox?")

   4. server applies the result (hit/miss) to the PRESENT world
      (e.g. deals damage now, even though the geometry test was in the past)
```

This is **favor-the-shooter**: "if it looked like a hit on your screen, it's a
hit." It's what Valve's Source engine documented in the seminal "Latency
Compensating Methods" write-up, and what virtually every modern shooter uses.

### 12.2 What the server must store, and the rewind cap

Lag compensation costs memory and introduces a fairness boundary:

- The server stores per-entity position/pose history deep enough to cover the
  worst latency it's willing to compensate — typically a few hundred ms.
- It **caps** the rewind: it will not honour a shot claiming to have been fired
  against a world more than, say, 200–250 ms in the past. Beyond the cap, the
  shot is evaluated more conservatively or rejected. The cap is the *direct
  control* on "how unfair can a behind-cover death be" (Chapter 14).

### 12.3 The cost made visible: "I was killed behind cover"

Rewind has a notorious side effect. Consider the *victim's* experience:

```
   victim's screen:  runs behind a wall at their t=0, feels safe
   shooter's screen: still sees the victim in the open (delayed view)
   shooter fires; server rewinds to the shooter's view → confirms a hit
   victim: dies a moment AFTER reaching cover, on their own screen
           → "I was already behind the wall! How did that hit me?!"
```

The victim is *correct* on their own screen and *also* legitimately dead. Both
experiences are internally consistent; they just describe different moments. This
is not a bug — it's the price of guaranteeing that the *shooter's* aim was
honoured. Favor-the-shooter necessarily means the *target* sometimes pays. We've
now hit the central tradeoff head-on; Chapters 13–16 dissect it.

---

## 13. The core conflict: whose "now" wins

Strip lag compensation to its essence and you find a decision that *cannot be
avoided*, only made:

> When two players interact across the latency gap, and they disagree about the
> state of the world (because each sees a different slice of time), **whose
> delayed reality does the server treat as authoritative for that
> interaction?**

There are exactly two pure answers, and a continuum between them.

### 13.1 Policy A — favor the shooter (lag compensation / rewind)

The server rewinds to what the *shooter* saw and judges the shot there
(Chapter 12).

> "If it looked like a hit on your screen, it's a hit."

- Hit registration feels *perfectly fair to the person aiming*. You shoot what
  you see; it connects.
- The cost is borne by the *target*, who can be hit after they believe they
  reached safety ("killed behind cover").
- **This policy is what creates peeker's advantage** (Chapter 14).

### 13.2 Policy B — favor the target (no rewind, present-tense hits)

The server judges every shot against the **current** authoritative world. What
the shooter saw is irrelevant; only where things *actually are now* counts.

> "Only your true present position can be hit."

- Hit registration feels fair to the person *being shot at* — you can never be
  hit somewhere you no longer are.
- The cost is borne by the *shooter*, who must **lead** every target by their
  ping; at high latency, hitting a moving target feels impossible and "I aimed
  dead-on and it didn't register" becomes the constant complaint.
- **This policy is what creates defender's advantage** (Chapter 15).

### 13.3 Why almost everyone picks A

Policy B's "lead by your ping" is so punishing that it makes a fast-paced shooter
feel broken for anyone above ~30 ms. Players overwhelmingly prefer "my aim
counts" over "my position is sacrosanct," so the industry standardized on
favor-the-shooter. But — and this is the crux — **choosing A does not escape the
tradeoff. It chooses peeker's advantage *on purpose* as the lesser evil.** You
are not removing unfairness; you are relocating it from the shooter to the
target, because you decided the shooter's experience matters more for your
game's feel.

---

## 14. Peeker's advantage, derived

**Peeker's advantage**: the player who moves around a corner (the *peeker*) can
see, and shoot, a stationary opponent (the *defender*/*holder*) before that
opponent's screen has even drawn the peeker. We can derive it precisely.

### 14.1 The timeline

```
   real time ────────────────────────────────────────────────────────►

   PEEKER (moving out from cover, sees their own position LIVE):
     t0  behind wall
     t1  steps out — on the peeker's OWN screen, the defender is right there
         (the peeker renders themselves with ZERO delay, Ch.8.3)
     t2  peeker fires at the defender

   the peeker's movement now travels to the server, then to the defender:
     t1 + (peeker upload) ............ server learns peeker stepped out
     ... + (defender download) ....... defender's CLIENT receives it
     ... + (defender interp buffer) .. defender's SCREEN finally draws the peeker

   DEFENDER (stationary, sees the peeker DELAYED + buffered, Ch.8):
     at the moment the peeker fired (t2), the defender's screen STILL shows
     an empty corner — the peeker hasn't "arrived" on their display yet
```

When the shot reaches the server, lag compensation (Policy A) rewinds to what the
*peeker* saw at t1/t2 — where the defender plainly was — and confirms the hit.
The defender is killed by someone who, *on the defender's own screen*, had barely
appeared or not appeared at all.

### 14.2 Why the defender is structurally late

Quantify the defender's disadvantage. The defender cannot react to the peeker
until the peeker is *drawn on the defender's screen*, which lags the peeker's
real action by:

```
   defender's reaction floor =  (peeker's upload latency)      ← peeker→server
                              +  (defender's download latency)  ← server→defender
                              +  (defender's interp buffer)      ← deliberate, Ch.8
                              +  (defender's own reaction time)  ← human, ~200ms
```

Meanwhile the peeker acted on a view that was *zero* frames delayed for their own
position. The peeker spends only their *own* reaction time; the defender spends
their reaction time **plus the entire network-and-buffer stack**. The defender is
behind by the sum of *both players' relevant latencies plus the interp buffer* —
they were never shown the target in time to win a reaction contest.

### 14.3 What makes it worse, what makes it better

From the formula:

- **Higher latency on either side** → worse (more delay before the defender sees
  the peeker).
- **Longer interpolation buffer** → worse (you chose to render remotes further in
  the past for smoothness; the peeker is one of those remotes).
- **Higher tick rate** → *better*, but bounded — it shrinks the quantization part
  of the gap, not the latency part (Chapter 3.3). It reduces peeker's advantage;
  it never removes it.
- **Rewind cap** (Chapter 12.2) → bounds the *worst case* but doesn't change the
  typical case.

### 14.4 It is a consequence, not a bug

Peeker's advantage is the **direct, unavoidable logical consequence** of
favor-the-shooter. You *traded for* it deliberately when you chose hit
registration that feels fair to the aimer. The only ways to reduce it are to
reduce the inputs to the formula (latency, buffer) or to shift partway toward
Policy B — which hands the advantage to the defender instead (Chapter 15). You
cannot have favor-the-shooter *and* no peeker's advantage; they are the same
thing.

---

## 15. Defender's advantage, derived

**Defender's advantage** is peeker's advantage's mirror image. It appears when
you *don't* fully favor the shooter, or when the buffers and policies tilt toward
the holder of a position.

### 15.1 Why holding an angle is structurally easier

```
   DEFENDER (stationary):
     - position is CONSTANT → their interpolated/delayed position is ACCURATE
       (a still target doesn't suffer from being shown in the past, Ch.8)
     - pre-aimed at the known choke point; reacting to a single expected angle
     - if the server checks hits in the PRESENT (Policy B) or with partial
       rewind, the defender's true position is exactly where everyone thinks
       it is → easy to hit FOR them, hard to be hit unfairly

   PEEKER (moving):
     - position is the one being interpolated/extrapolated → UNCERTAIN on the
       defender's screen
     - under present-tense hit checks, the peeker's true (moving) position is
       what the defender's shots are tested against — the peeker can't fully
       escape it
```

A stationary defender's *position is certain*; a moving peeker's *position is
uncertain*. Under any policy that doesn't fully rewind to the peeker's view, the
certain party wins the exchange. The defender also enjoys *preparation* — they
chose the angle, pre-aimed, and need only react to one expected event, while the
peeker must acquire, identify, and fire.

### 15.2 The advantage switches sides with the policy

The decisive observation:

> The same latency gap that *helps the peeker* under favor-the-shooter *helps the
> defender* under favor-the-target. Changing the policy does not delete the
> advantage — it **moves it to the other player.**

This is why "fixing" peeker's advantage by removing lag compensation doesn't
produce a fair game; it produces a game with the *opposite* unfairness, where
aggressive play is punished and holding angles is dominant. Many tactical
shooters lean slightly this way on purpose (favoring deliberate, defensive play);
many fast arena shooters lean hard the other way (favoring aggression). Neither
is "more correct" — they're different points on one axis, chosen to fit the
game's intended feel.

---

## 16. The unified tradeoff and the tuning axis

Put the two advantages side by side and the structure is unmistakable: **they are
one tradeoff seen from opposite ends.**

| | Server rewinds to shooter's view | Server checks present truth |
|---|---|---|
| **Policy name** | Favor the shooter (lag compensation) | Favor the target |
| **Who is advantaged** | The **peeker** (the mover, the aggressor) | The **defender** (the holder, pre-aimed) |
| **Hit-reg feels fair to** | The person aiming | The person being shot at |
| **The complaint you'll hear** | "I got shot behind the wall / before I even saw them." | "I hit them dead-on and nothing registered." |
| **Encourages** | Aggressive, mobile play | Deliberate, positional play |
| **Used by** | Most modern shooters | Rare as a *pure* policy; partial in some tac-shooters |

And the axis you actually tune along:

```
        ◄──── favor the target                 favor the shooter ────►
   DEFENDER'S ADVANTAGE                                  PEEKER'S ADVANTAGE
        ●───────────────────────┼───────────────────────────●
   "you can't be hit       your design's       "your aim always
    where you no longer     tuning point        counts, even against
    are"                    lives somewhere      someone who's already
                            on this line         taken cover"

   moving the point LEFT  → silences "shot behind cover", angers aggressors
   moving the point RIGHT → silences "my shots don't register", angers holders
```

You are **always somewhere on this axis** — there is no point labelled "fair to
everyone." Every adjustment that quiets one set of complaints amplifies the
other. The realistic goal is not elimination but **placement and consistency**:

1. **Placement** — put the residual unfairness where it least harms *your* game's
   intended experience (aggression-rewarding vs. position-rewarding).
2. **Consistency** — keep it stable and predictable so players can *learn* it and
   incorporate it into their skill. A consistent peeker's advantage becomes
   "pre-aim and pre-fire angles"; an inconsistent one just feels like the game is
   broken.

> The art of netcode combat is not making latency disappear (it can't) but
> **choosing, on purpose and consistently, who pays for it — and making that bill
> feel fair.**

---

## 17. The levers and their costs

Every design control, what it moves you toward, and what it costs. Nothing here
is free; that's the point.

| Lever | Pushes toward | What it costs |
|---|---|---|
| **Lag compensation / server rewind** | Peeker (shooter) | "Killed behind cover" moments; server must store position history; opens lag-comp-specific exploits (fake-lag) if unbounded. |
| **No rewind / present-tense hits** | Defender (target) | Must-lead-your-target; feels broken above modest ping; punishes aggression. |
| **Rewind window cap** | Bounds the worst case toward defender | Players above the cap get worse hit-reg; choosing the cap is itself a fairness call. |
| **Interpolation buffer length** | Longer = more peeker's adv.; shorter = less | Longer = smoother, more loss-tolerant remotes. Shorter = jittery, freeze-prone remotes. Pure smoothness↔fairness trade. |
| **Tick rate** | Higher = smaller gap, both directions | CPU + bandwidth scale linearly; diminishing returns below the jitter floor; reduces but never removes the gap. |
| **Client-side prediction** | Makes *your own* actions feel instant | Mispredictions → visible corrections/rubber-banding; requires shared sim code + reconciliation. |
| **Server-authoritative movement** | Cheat resistance, single source of truth | You *must* predict + reconcile or the local player feels laggy; more server CPU. |
| **Client-authoritative movement** | Trivially responsive | Trusts the client about its own position → speedhacks/teleports; competitively unsafe. |
| **Send rate vs. tick rate decoupling** | Bandwidth control | Sending less often than you simulate saves bandwidth but coarsens remote motion (more interp work). |
| **Delta-from-acked baselines** | Robustness under loss | Per-peer history + ack bookkeeping; more server memory and complexity than diff-from-sent. |
| **Area-of-interest / relevancy** | Scale (more players/objects) | Risk of "pop-in" at relevancy edges; cost of computing relevancy each tick (Chapter 19). |
| **Peeker's-advantage mitigations** | Toward defender, partially | e.g. slightly delaying the peeker's *own* view, or "shoot-back time" windows. Controversial; subtly changes feel for everyone. |

The table is the practical payoff of the whole document: faced with a netcode
complaint, you locate the lever, predict which way it moves the axis, and accept
the cost it imposes elsewhere — rather than chasing a free fix that doesn't
exist.

---

## 18. Alternative architectures: lockstep and rollback

The authoritative-server model (Chapters 4–17) is dominant but not universal.
Two other families matter, especially for fighting games, RTS games (*real-time
strategy* — e.g. *StarCraft*, where one player commands hundreds or thousands of
units), and other deterministic genres.

### 18.1 Deterministic lockstep

Instead of replicating *state*, replicate only *inputs*. Every machine runs the
*same* simulation deterministically, so given the same inputs they compute the
same world. Used heavily by RTS (thousands of units — replicating each unit's
state would be impossible; replicating a handful of player commands is cheap).

```
   every machine:  collect local input for tick N
                    exchange inputs with all peers
                    once ALL inputs for tick N are in → simulate tick N
                    (identical inputs + identical sim ⇒ identical world)
```

- **Bandwidth:** tiny and *constant* regardless of world size — you send commands,
  not state. This is lockstep's superpower.
- **Latency:** brutal. You cannot simulate tick N until *every* player's input for
  N has arrived. The whole game runs at the speed of the *slowest* connection,
  and one stalled player stalls everyone. Mitigated with **input delay** (apply
  inputs a few ticks after they're issued, so they have time to propagate) — which
  trades responsiveness for tolerance.
- **Fragility:** the entire model rests on perfect determinism. Any divergence
  (Chapter 11.3) is fatal and unrecoverable, which is why lockstep games obsess
  over fixed-point math, deterministic iteration, and state checksums.

### 18.2 Rollback netcode (GGPO-style)

Rollback is lockstep plus *prediction* applied to remote inputs. Instead of
stalling until a remote input arrives, you **predict** it (usually "same as last
frame"), simulate forward immediately, and when the real input arrives:

```
   predicted remote input for frames N..N+3 (assume unchanged)
   real input for frame N arrives, DIFFERENT from prediction →
     1. ROLL BACK the simulation to frame N
     2. re-apply the CORRECT input at N
     3. re-simulate N+1, N+2, N+3 at once (fast — no rendering)
     4. resume; the screen jumps to the corrected present
```

- **Feel:** local inputs are *instant* (you never wait), and most predictions are
  correct (players hold inputs across frames), so rollbacks are usually 1–2
  frames and invisible.
- **Cost:** you must be able to *save and restore* full game state cheaply and
  re-simulate several frames within one frame's budget — demanding, and it forces
  the entire game to be rollback-safe (no side effects you can't undo).
- **Where it shines:** fighting games, where two players, frame-perfect
  responsiveness, and short rollbacks make it near-magical (this is why GGPO
  changed the genre).

### 18.3 How to choose

| | Authoritative server | Lockstep | Rollback |
|---|---|---|---|
| **Replicates** | State (deltas) | Inputs | Inputs + local prediction |
| **Bandwidth** | Scales with state | Tiny, constant | Tiny, constant |
| **Players** | Many | Many (RTS) | Usually 2 (fighting) |
| **Cheat-resistant** | Yes (server validates) | No (every peer simulates all) | No (peer-to-peer) |
| **Failure mode** | Reconciles continuously | Desync = fatal | Desync = fatal; rollback hitches |
| **Best for** | Shooters, MMOs, co-op | RTS | Fighting games |

(*MMO* = massively-multiplayer online game — a persistent world with hundreds or
thousands of concurrent players, e.g. *World of Warcraft*.)

The authoritative-server model is the right default for most 3D action games
because it tolerates loss gracefully, resists cheating, and doesn't demand
perfect determinism. Lockstep/rollback win when bandwidth must be tiny and the
game can guarantee determinism.

### 18.4 The fixed-timestep prerequisite

All three models — and even plain client prediction (Chapter 11.5) — are far more
robust on a **fixed simulation timestep**. If the simulation advances by a
variable `dt` each frame, two machines (or a client and a server) integrating the
same motion with different frame rates get different results, seeding
determinism drift and reconciliation jitter. A fixed-step accumulator decouples
simulation from render rate:

```
   accumulator += real_dt
   while accumulator >= FIXED_DT:
       simulate one fixed step
       accumulator -= FIXED_DT
   render with interpolation between the last two fixed states
```

This is why mature netcode roadmaps treat "fixed-step simulation" as a *hard
gate* before server-authoritative movement, rollback, or lockstep: those
techniques are unsound on a variable timestep.

---

## 19. Bandwidth, relevancy, and scale

Latency is the glamorous constraint; **bandwidth** is the one that decides how
many players and objects you can support. This chapter covers the techniques for
fitting a large world through a small pipe.

### 19.1 The bandwidth budget

A player's downstream is finite and shared with everything else they're doing.
The state you can send is bounded by:

```
   bytes/sec  =  relevant_objects × bytes_per_object × send_rate
```

Every factor is a lever:

- **`relevant_objects`** — area-of-interest culling (19.3).
- **`bytes_per_object`** — quantization and delta compression (19.2).
- **`send_rate`** — can be lower than the tick rate; coarser remote motion is the
  cost.

Blow the budget and you get queueing delay (bufferbloat, Chapter 1), loss, and
the desync that loss causes. Bandwidth discipline is not optional at scale.

### 19.2 Compression of state

- **Delta compression** (Chapter 10) — send only changed fields. The single
  biggest win; idle objects cost nothing.
- **Quantization** — *quantizing* means snapping a value to a limited set of
  steps so it fits in fewer bits. Don't send full 32-bit floating-point numbers
  for a position when the world is 4 km across and 1 cm precision suffices; pack
  it into far fewer bits. Rotations are commonly stored as *quaternions* (a
  four-number encoding of an orientation) and compressed with the *smallest-three*
  trick: since a rotation quaternion's four components always satisfy a fixed
  relationship, you can drop the largest one and reconstruct it on the other
  side, saving a quarter of the bits. Quantization is lossy but imperceptible if
  the step size is chosen well.
- **Bit-packing and entropy coding** — pack booleans and small enums into bits;
  optionally entropy-code the result.
- **Baseline choice** — delta-from-acked (Chapter 10.3) trades memory for
  loss-robustness.

### 19.3 Relevancy / area of interest

The decisive idea for large worlds: **a client doesn't need state for things it
can't perceive.** Cull replication by relevancy:

- **Distance / PVS** — only replicate objects near the player or in their
  potentially-visible set. An MMO replicates your neighbourhood, not the
  continent.
- **Priority** — when the budget is tight, send the *most important* objects more
  often (the enemy shooting at you) and distant/idle ones less often.

The cost is **edge artifacts**: an object popping into relevance must be spawned
(reliably), and a player at a relevancy boundary can see things flicker in and
out. Relevancy is what turns "a few players in a room" into "hundreds of players
in a world," and it's the architectural difference between a deathmatch and an
MMO.

### 19.4 Send rate, prioritization, and fairness

Decoupling send rate from tick rate lets you simulate at a high, accurate rate
while shipping updates at a sustainable one. Combined with priority, a tight
budget degrades *gracefully*: nearby/important state stays crisp while distant
state coarsens, rather than everything stuttering uniformly. This graceful
degradation is itself a design goal — a player on a poor connection should lose
fidelity on things that don't matter before things that do.

---

## 20. Security: the client is in enemy hands

A foundational assumption, easy to forget: **the client runs on a machine the
player fully controls.** Memory can be read and written, packets inspected and
forged, the binary patched. Anything the client is *trusted* to decide can be
cheated. This is why authority lives on the server (Chapter 4) — but the
implications run deeper.

### 20.1 The trust boundary

The only trustworthy code is code the *server* runs and the *server* validates.
Everything the client sends is a *request*, not a *fact*. Concretely:

- **Never trust client-reported outcomes.** "I hit them" / "I picked it up" / "my
  health is X" must all be *decided* by the server. A client that reports its own
  hits can report hitting through walls, from across the map, every shot.
- **Validate, don't assume, client *inputs*.** Even inputs can be abused: an
  input claiming impossible movement (speed beyond the max, teleporting,
  no-clipping through geometry) must be *rejected or clamped* by the server, not
  applied. Client-predicted movement is fine; client-*dictated* movement is not.
- **Don't send what the client shouldn't know.** Relevancy (Chapter 19.3) is also
  a security control: if you don't replicate an enemy's position because the
  player can't see them, a wallhack has nothing to read. Sending state "for
  convenience" and hiding it only in the renderer is a classic information leak
  exploited by maphacks.

### 20.2 Lag-comp-specific abuse

Lag compensation (Chapter 12) trusts the client's claim about *when* it fired.
Unbounded, this invites **fake-lag / lag-switch** abuse: artificially inflating
your latency so the server rewinds further, letting you hit opponents who have
long since moved. The rewind cap (Chapter 12.2) is partly a *security* control,
not just a fairness one: it bounds how far a manipulated latency can rewind the
world.

### 20.3 Protocol hardening

- **Bounds-check every field** on receive. A length or index from the wire is
  attacker-controlled; an unchecked one is a buffer overflow or crash. Treat the
  wire as hostile input.
- **Version the protocol** in the handshake so mismatched builds are rejected
  cleanly rather than misinterpreting each other's bytes (a desync *and* a
  crash vector).
- **Rate-limit and validate the handshake** so a malicious peer can't exhaust
  server resources or smuggle a malformed connection through.

Security is not a late-stage bolt-on; it's a consequence of the authority model
chosen in Chapter 4. A client-authoritative design *cannot* be secured after the
fact — the trust was given away at the architecture level. This is the strongest
non-feel argument for server authority.

---

## 21. Measurement: you cannot tune what you cannot see

Every tradeoff in this document is *quantitative* — buffer lengths, tick rates,
rewind caps, bandwidth budgets. Tuning them by feel, on a LAN, is guessing. The
final discipline is **measurement and reproduction.**

### 21.1 Observe the live link

Continuously surface, per connection:

- **RTT** and its trend — how stale remote data is.
- **Jitter** (latency variance) — the thing your interp buffer must absorb.
- **Packet loss** — the thing that triggers desync and extrapolation.
- **Bandwidth** in/out, packets/sec — the budget you're spending.

Without these visible, "the game feels laggy sometimes" is undiagnosable. With
them, you can see *which* of latency, jitter, loss, or budget is the culprit, and
reach for the right lever.

### 21.2 Reproduce bad networks on a good one

A LAN has ~0 ms latency, ~0 jitter, ~0 loss — i.e. the *one environment your game
will never ship into*. A **conditioning simulator** that injects artificial
latency, jitter, and loss into the local transport lets you reproduce a 150 ms /
5 %-loss link on two machines on the same desk, *deterministically*, on demand.

This is the instrument that makes every other claim trustworthy:

- It validates interpolation (does motion stay smooth at 80 ms jitter?).
- It validates reconciliation (does the local pawn stay stable under loss?).
- It exposes dropped-edge desync (Chapter 11.2) — which is *invisible on a LAN*
  because nothing drops there, and only appears under real loss.
- It lets you tune buffers and caps against representative conditions before you
  have any internet infrastructure at all.

> **The governing rule of any netcode roadmap: don't build reach you can't
> validate.** The conditioning simulator is the measurement tool that gates
> everything built on top of it. Building NAT traversal (the techniques needed to
> connect two machines that each sit behind a home router — the router performs
> *Network Address Translation*, hiding the machine's real address, so getting an
> unsolicited inbound connection through it takes extra work), dedicated servers,
> or rollback *before* you can reproduce and measure a degraded link is building
> on sand — you won't know if it works until players on bad connections tell you
> it doesn't.

---

## 22. Worked end-to-end example

To cement the whole stack, follow a single shot from trigger-pull to confirmed
kill across two players, A (the shooter/peeker) and B (the target/defender), each
at 50 ms RTT to an authoritative server, with a 2-tick (~66 ms at 30 Hz) interp
buffer.

```
   ── A's machine ───────────────────────────────────────────────────────
   A renders B via INTERPOLATION, ~66ms + 25ms = ~91ms in the past (Ch.8)
   A renders A's OWN pawn via PREDICTION, in the present (Ch.6) — zero lag
   A steps around the corner; on A's screen, B is standing in the open

   A pulls the trigger. A's client:
     - predicts the firing animation locally (instant feedback, Ch.6)
     - sends a "fire" command stamped with A's render time T_see
       (the server-time A's screen was depicting: server_now − 25 − 66)
       and A's aim ray                                       ─┐ in flight
                                                               │  ~25ms
   ── server ────────────────────────────────────────────────┘
   server receives the fire command:
     - reads T_see from the packet (clamped to the rewind cap, Ch.12.2)
     - REWINDS B (and all relevant entities) to their pose at T_see,
       looked up from the position history buffer (Ch.12.1)
     - tests A's aim ray against B's REWOUND hitbox → HIT
       (B was plainly in the open at T_see, even though B has since moved)
     - applies damage to B in the PRESENT world (Ch.12.1 step 4)
     - this is FAVOR-THE-SHOOTER → A wins the exchange (Ch.13.1)
     - server queues: B's new health (state, unreliable delta, Ch.10.2),
       a hit/kill event (transition, RELIABLE, Ch.10.4)              ─┐
                                                                       │ ~25ms
   ── B's machine ───────────────────────────────────────────────────┘
   B receives the updates:
     - B's screen, until now, STILL showed an empty corner (A's movement
       was delayed by A-upload + B-download + B's interp buffer, Ch.14.2)
     - B now sees A appear AND sees themselves take fatal damage,
       seemingly simultaneously → "I got shot before I even saw them!"
     - B is correct on their own screen AND legitimately dead (Ch.12.3)

   ── what just happened, in the vocabulary of this document ─────────────
   • Three clocks never aligned (Ch.3): A acted on a ~91ms-old view of B.
   • A predicted locally (Ch.6) so firing felt instant.
   • B was interpolated on A's screen (Ch.8): smooth but delayed.
   • The server favored the shooter via rewind (Ch.12,13): A's aim counted.
   • B paid the cost as peeker's advantage (Ch.14): structurally too late.
   • The kill event went reliably; B's health went as an unreliable delta
     against an acked baseline (Ch.10) so a dropped packet wouldn't desync
     B's health permanently (Ch.11.2).
   • None of this is a bug. Every step is a deliberate choice about WHO PAYS
     for the unclosable latency gap (Ch.1,16).
```

That single shot exercises every major idea in the document. If you can narrate
why each step is the way it is — and what would change if you moved any lever —
you understand netcode.

---

## 23. Glossary

- **Authority** — the right to write an object's *true* state. In the dominant
  model, the server has it for everything.
- **Possession / ownership** — which *player* drives an object (camera, input).
  Distinct from authority (Chapter 4.2).
- **Baseline** — the reference state a delta is computed against. "Last sent" vs.
  "last acked" matters under loss (Chapter 10.3).
- **Client-side prediction** — the client immediately simulates its own input
  rather than waiting for the server (Chapter 6).
- **Dead reckoning** — extrapolating an entity's future from its last known
  velocity (Chapter 9).
- **Delta compression** — sending only changed fields against a baseline
  (Chapter 10.2).
- **Desync** — a non-self-correcting disagreement about world state
  (Chapter 11).
- **Entity interpolation** — rendering remote entities in the past, between two
  known samples, for smooth motion (Chapter 8).
- **Extrapolation** — predicting beyond the last known sample; a bet, not a fact
  (Chapter 9).
- **Favor the shooter / favor the target** — the two policies for whose delayed
  view wins a contested hit (Chapter 13).
- **Fixed timestep** — advancing the simulation by a constant `dt`; a
  prerequisite for robust prediction, lockstep, and rollback (Chapter 18.4).
- **Head-of-line blocking** — TCP holding delivered packets hostage behind one
  lost packet (Chapter 2.2).
- **Interpolation buffer / interp delay** — how far in the past remotes are
  rendered; absorbs jitter, trades latency for smoothness (Chapter 8.1).
- **Jitter** — variance in latency; often worse than raw latency (Chapter 2.4).
- **Lag compensation** — server-side rewind to the shooter's view to judge a hit
  (Chapter 12).
- **Listen-server** — a server that runs inside one of the players' clients
  (Chapter 0, Chapter 4.2).
- **Lockstep** — replicate inputs, simulate identically everywhere
  (Chapter 18.1).
- **Peeker's advantage** — the mover sees and shoots the holder first under
  favor-the-shooter (Chapter 14).
- **Defender's advantage** — the holder wins under favor-the-target or partial
  rewind (Chapter 15).
- **Reconciliation** — correcting a prediction by accepting the server's past and
  replaying pending inputs (Chapter 7).
- **Relevancy / area of interest** — only replicating what a client can perceive,
  for scale and security (Chapter 19.3, 20.1).
- **Rewind cap** — the maximum time the server will rewind for lag comp; a
  fairness *and* security control (Chapter 12.2, 20.2).
- **Rollback** — lockstep with predicted remote inputs and re-simulation on
  misprediction (Chapter 18.2).
- **RTT** — round-trip time; one-way latency is ~RTT/2 but not guaranteed
  symmetric (Chapter 0).
- **Shadow copy** — the server's record of the last state it sent, used to
  compute deltas (Chapter 10.2).
- **Snapshot** — a full or partial picture of world state at a tick
  (Chapter 10.1).
- **Tick rate** — how often the simulation advances; a bounded lever on the
  latency gap (Chapter 3.3, 17).

---

## 24. Further reading and lineage

The techniques here are decades old and well documented. The canonical primary
sources, roughly in order of influence:

- **Quake / QuakeWorld (1996–97)** — the origin of client-side prediction over a
  UDP authoritative server. John Carmack's `.plan` files and the QuakeWorld
  source are where "predict locally, the server is right" entered games.
- **Valve, "Source Multiplayer Networking" / "Latency Compensating Methods in
  Client/Server In-game Protocol Design and Optimization" (Bernier, 2001)** — the
  definitive write-up of lag compensation (server rewind), entity interpolation,
  and the interp-delay tradeoff. The source of much of Chapters 8, 12, 14.
- **Quake 3 networking model** — the snapshot + delta-against-acked-baseline
  design that Chapter 10.3 describes; the reference for robust delta compression
  under loss.
- **Gabriel Gambetta, "Fast-Paced Multiplayer" series** — the clearest modern
  pedagogical treatment of prediction, reconciliation, and interpolation, with
  interactive demos. The shape of Chapters 5–8 follows its logic.
- **Glenn Fiedler, "Gaffer on Games"** — deep, practical articles on UDP
  transport, reliability over UDP, snapshot compression, and fixed-timestep
  integration. Chapters 2, 10, 18.4 lean on this lineage.
- **GGPO / Tony Cannon's rollback work** — the foundation of modern fighting-game
  netcode and the canonical rollback reference (Chapter 18.2).
- **Tribes networking model (Frohnmayer & Gift)** — an early, influential
  treatment of prioritized, bandwidth-budgeted state replication and relevancy
  (Chapter 19).

The vocabulary and the *tradeoffs* are stable across all of them. Implementations
differ; the physics in Chapter 1 and the "who pays for the gap" framing in
Chapter 16 do not.

---

*End. If you take one thing from this document, take Chapter 16: latency cannot
be removed, only assigned. Every technique above is a different, deliberate
answer to the question "who pays for the gap, and how do we make the bill feel
fair?"*
