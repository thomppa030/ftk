# Networking

The complete picture of Fjell's multiplayer: what runs today, and the staged
plan for everything that follows. Written so you can read it top-to-bottom and
come away with a mental model — and so it can drive a visual diagram.

---

## 1. The shape of it

Fjell multiplayer is a **listen-server**: one machine is both a player and the
authority. Everyone else is a client that connects to it. There is no separate
dedicated server today (that's a v3 option).

```
            ┌──────────────────────────────────────────────┐
            │  HOST  (listen-server)                         │
            │  ┌──────────┐   authoritative world state      │
            │  │ NetManager│  ── spawns, owns NetIds,         │
            │  └────┬─────┘     resolves all replication      │
            │       │ Transport (ENet, UDP)                   │
            └───────┼────────────────────────────────────────┘
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
   ┌────────┐  ┌────────┐  ┌────────┐
   │ Client │  │ Client │  │ Client │   predict locally, interpolate
   │   A    │  │   B    │  │   C    │   remotes, reconcile to host
   └────────┘  └────────┘  └────────┘
```

Two ideas underpin everything and are easy to confuse — keep them apart:

| Concept | Question it answers | True when… |
|---|---|---|
| **Authority** (`is_local_authority`) | "May I write this object's *real* state?" | Server: for **every** object. Client: only for objects it **owns**. |
| **Possession** (`is_owned_by_local`) | "Does the local *player* drive this?" (camera, input) | Only when `owner == my peer id`. |

The host is authoritative over the whole world but only *possesses* the objects
it spawned for itself. A client possesses its own pawn but has authority over
nothing it doesn't own. Camera/input gating uses **possession**; replication
write-gating uses **authority**.

---

## 2. Core vocabulary

| Term | Type | Meaning |
|---|---|---|
| `PeerId` | `uint32_t` | A connection. `SERVER_PEER = 0` is always the host; clients get `1..N` after the handshake. |
| `NetId` | `uint32_t` | A networked object's shared identity. Server-assigned; `0` = none. |
| `NetRole` | enum | `none` (offline) / `server` / `client`. |
| `ConnectionState` | enum | `offline` / `connecting` / `connected` / `failed` / `disconnected`. The real link state, distinct from intent. |
| `NetLaunchMode` | enum | `offline` / `host` / `join`. The editor's *intent*, read by the game script. |
| `Channel` | enum | `reliable` (ordered, guaranteed) / `unreliable` (latest-wins, may drop). |

**Launch is intent, not action.** The editor's Net dropdown sets `launch_mode`
and `join_address` on the NetManager. The engine *never* connects on its own —
the **game script** reads these in `begin_play` and decides whether to call
`host()`, `join()`, or stay offline. This keeps the engine game-agnostic.

---

## 3. What exists today (built and shipping)

### The transport seam

Everything above the transport speaks only in opaque byte buffers + `PeerId`s.
That seam is what lets a future Steam backend drop in without touching gameplay.

```
        NetManager
            │  send(peer, bytes, channel) / broadcast / poll
            ▼
     ┌─────────────┐         interface: transport.hpp
     │  Transport  │  ◄──────  host/connect/disconnect, send/broadcast,
     └──────┬──────┘           poll, flush, stats, peers, connection_state,
            │                  set_conditioning + on_connect/disconnect/receive
     ┌──────▼───────┐
     │ EnetTransport│  the only implementation today (ENet over UDP)
     └──────────────┘
```

`EnetTransport` maps each `PeerId` onto an `ENetPeer`, allocates client ids
sequentially from 1, enforces a 5-second connect timeout, and hosts the
**network conditioning** simulator (artificial latency / jitter / loss) used to
validate the netcode from one machine.

### NetManager and its six parts

`NetManager` is a thin facade. The real work lives in six focused components
behind it — the result of decomposing what was once a 1000-line class:

```
                         ┌───────────────┐
                         │   NetManager   │  facade: lifecycle, per-frame pumps,
                         └───────┬────────┘  authority/possession, launch mode
        ┌──────────┬──────────┬─┴────────┬───────────┬────────────┐
        ▼          ▼          ▼          ▼           ▼            ▼
   ┌─────────┐┌──────────┐┌──────────┐┌─────────┐┌───────────┐┌───────────┐
   │NetScene ││Replication││Prediction││  Spawn  ││ RpcRouter ││PacketCodec│
   │         ││  Graph    ││  State    ││Replicator│          ││           │
   └─────────┘└──────────┘└──────────┘└─────────┘└───────────┘└───────────┘
   identity   shadow-diff  interp +    spawn /     [[fj::rpc]]  wire format
   ownership  delta state  reconcile   despawn +   routing      (put/take,
   authority  replication  remotes     late-join                PacketType)
```

| Component | Owns / does |
|---|---|
| **NetScene** | NetId ↔ node maps, ownership, the two authority/possession queries. The single source of truth for identity. |
| **ReplicationGraph** | Per-object shadow copies; diffs replicated fields + transform each tick; builds/applies `state_update`. |
| **PredictionState** | Render-time interpolation of remote objects; smoothed reconciliation of owned (predicted) objects. The subtlest code. |
| **SpawnReplicator** | Server spawn/despawn with NetId assignment; replays spawn history so late joiners catch up. |
| **RpcRouter** | Routes `[[fj::rpc]]` calls by target (server / clients / all); relays on the server. |
| **PacketCodec** | Pure, stateless serialization. The *only* place that knows the wire layout. |

### How a frame moves data

Two subsystems bracket the tick:

```
  ── frame start ─────────────────────────────────────────────────────►
  NetReceiveSubsystem (early, before scripts)
     transport.poll()  →  receive_and_apply()  →  interpolate_remotes()
        drain UDP          parse + apply           smooth remotes toward
        events             state/spawn/rpc         net_time − interp_delay

  … scripts run, physics runs, world matrices computed …

  NetSendSubsystem (late, after physics) — fires at a fixed rate (default 30 Hz)
     tick_beacon()   →   collect_and_send()
        LAN advert        diff owned objects vs shadow, ship only changes
  ◄──────────────────────────────────────────────────────────── frame end ──
```

### Replication: shadow-copy delta compression

Each send tick the authority walks the objects it owns, compares each
`[[fj::replicated]]` field + transform against a stored **shadow** (last-sent
bytes), and ships only what changed on the **unreliable** channel. Spawn,
despawn, RPCs and the welcome handshake go **reliable**.

On the receiving side:
- **Remote objects** feed `PredictionState` — an 8-sample ring on a synthetic
  timeline. The rendered transform is interpolated ~2 ticks behind the newest
  sample, so motion stays smooth between discrete 30 Hz snapshots.
- **Owned objects** (predicted locally) record a correction: hard-snap if the
  error is large, otherwise ease toward the server's value.
- The **server relays** client-authored updates on to the other clients.

### Packet types

`state_update` (1) · `spawn` (2) · `despawn` (3) · `rpc_call` (4) · `welcome` (5).
The welcome packet is how a freshly-connected client learns its assigned
`PeerId`.

### LAN discovery (shipped most recently)

Separate from ENet — a raw UDP broadcast:

```
   HOST                                   CLIENT (scanning, offline)
   DiscoveryBeacon                        DiscoveryListener
   every 1s broadcast on :47777   ──────► binds :47777, drains datagrams
   {magic 'FJBD', version, name,          dedups by address+port,
    game_port, players, max}              ages out after 3s unheard
                                          → server list in the net panel
```

Clicking a discovered server sets `launch_mode = join` + `join_address` — the
same thing the manual Join box does. **LAN only**: broadcast doesn't cross
routers, and won't loop back to a listener on the *same* machine (so verifying
discovery needs two boxes).

### Script-facing API (`fjell/net_api.hpp`)

Games never touch the engine types directly. They get a mirrored surface:
queries (`role`, `is_server`, `connection_state`, `launch_mode`, `join_address`,
`is_local_authority`, `is_owned_by_local`), lifecycle (`host`, `join`,
`disconnect`), `spawn`/`destroy`/`set_owner`, and the generated RPC send path.

### The net panel

An ImGui debug panel: role + connection badge, host's LAN addresses (with copy),
the LAN server list, per-second bandwidth + packet rates with a sparkline, the
connected-peer table (RTT / loss), and the conditioning sliders.

---

## 4. The roadmap

Four versions, each a hard gate on the last. The governing rule: **don't build
reach you can't validate.** The conditioning simulator (v2 Phase 1) is the
measurement instrument that makes every later claim trustworthy.

```
  v1 ──────────► v2 ──────────► v3 ──────────► v4
  local play     beyond local   shippable      Steam
  (DONE)         (DONE)         game            backend
                                (planned)       (planned)
```

### v2 — Beyond local play ✅ DONE

| Phase | What | Status |
|---|---|---|
| **1 — Observability + conditioning** | Transport stats, our own latency/jitter/loss simulator, the net panel. The testing tool that gates everything after. | ✅ |
| **2 — Non-local play first-class** | 2a editable+persisted join target & host LAN IP · 2b LAN discovery beacon · 2c connection lifecycle hardening (state enum, "host left"). | ✅ |
| **3 — Internet play** | 3a port-forward WAN spike (manual, zero infra) · 3b NAT-traversal *decision* (STUN+punch / TURN relay / third-party) · 3c security pass (gate before public exposure). | ◻ next |
| **4 — Architecture options** | Dedicated server, full rollback, area-of-interest, transform compression — *only when a real game demands them.* Mostly promoted into v3 below. | ◻ as-needed |

> **You are here.** v2 Phases 1–2 are complete. The next buildable step is
> **3a** — but it's a *cross-internet* test (two different networks). Same-WiFi
> machines validate LAN (Phase 2) for real but prove nothing about NAT.
> **3c** (security) and the v3 bandwidth work are testable single-machine via
> the conditioning sim.

### v3 — Shippable game (planned, not started)

Gameplay-replication depth + production scale. Gated on a deterministic loop.

- **Phase 0 (hard gate) — Fixed-step simulation.** Accumulator-based fixed
  timestep so server-auth physics, rollback, and lockstep become possible.
  *Gates 3a/3b/3c below.*
- **Phase 1 — Replication authoring (designer-facing).**
  `[[fj::replicated(owner_only)]]` / `(on_change)`, RepNotify `on_rep_<field>()`
  callbacks, authority as a first-class script concept
  (`has_authority`/`request_ownership`/`assign_ownership`), optional reliable
  event bus.
- **Phase 2 — Bandwidth & scale.** Transform quantization + delta-from-acked,
  area-of-interest relevancy, per-peer acks (also fixes a known ~3%-loss
  animation-param artifact), priority/bandwidth budgeting.
- **Phase 3 — Authority models.** Server-authoritative movement (anti-cheat
  baseline), full rollback re-simulation, dedicated headless build, and
  deterministic lockstep noted as a far-future non-goal.
- **Phase 4 — Production hardening.** Reconnect/rejoin, protocol versioning in
  the handshake, defensive input validation, persistence hooks, telemetry.

### v4 — Steam backend (planned, additive)

A *second* `Transport` implementation — ENet stays the default forever. The
promise: if a `SteamID` or `#ifdef FJELL_WITH_STEAM` ever appears in
`net_manager.*` or above, the abstraction is broken.

```
        NetManager  ── backend enum (like the SSR mode toggle) ──┐
            ▼                                                     ▼
     ┌──────────────┐                                   ┌────────────────┐
     │ EnetTransport│ default · LAN · offline · dev     │ SteamTransport │ opt-in,
     └──────────────┘                                   └────────────────┘ falls back
                                                          ISteamNetworkingSockets:
                                                          P2P + NAT + relay for free
```

- **Phase 0 — SDK plumbing.** `FJELL_WITH_STEAM` CMake flag (off by default),
  init/shutdown/`RunCallbacks`, all Steam headers walled inside
  `steam_transport.*`.
- **Phase 1 — SteamTransport.** Implement the `Transport` interface over
  `ISteamNetworkingSockets`; keep `PeerId ↔ CSteamID` mapping *inside* the
  backend. The one interface change allowed: an additive `connect_steam`
  (don't smuggle SteamIDs through the address string).
- **Phase 2 — Auth & identity.** Steam auth tickets, persona names in the peer
  list. Identity verification only — not behavioral anti-cheat.
- **Phase 3 — Lobbies & social.** `ISteamMatchmaking` lobbies, friend invites
  via the overlay, "Join game" from a profile. Supersedes LAN discovery for
  *internet* play; LAN beacon stays for offline.

---

## 5. The through-line

The same invariants survive from v1 to v4 and are the things to never regress:

1. **The transport seam holds.** Nothing above `Transport` knows ENet from
   Steam. Backends are interchangeable.
2. **Authority ≠ possession.** Defined once, in NetScene; the right predicate
   gates the right thing.
3. **Symmetric interpolation + join/idle anchoring.** The subtle PredictionState
   behavior that makes remote motion smooth — moved whole, never "improved" in
   passing.
4. **Validate before you reach.** The conditioning simulator proves the netcode
   under real conditions before any infrastructure is built on top of it.
