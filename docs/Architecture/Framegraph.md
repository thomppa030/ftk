# Framegraph

> Stub — fill in.

## Scope
The DAG-based frame builder. Passes declare reads/writes; the framegraph schedules them, batches barriers, aliases transient memory, and overlaps compute on the async queue. Authored via `.fjgraph` files and registered passes.

## Planned sections
- Why DAG — vs hand-ordered passes, vs Frostbite-style
- `PassRegistry` + `pass_registrations.cpp` — self-registering passes
- `PassSetup` / `PassBuilder` — declaring inputs/outputs, virtual resources
- `ResourceRegistry` — physical resource pool
- `RenderPhase` enum — high-level ordering bucket
- Aliasing — same-size, compatible-format transient sharing
- Barrier batching
- Async compute
  - When a pass goes on the compute queue
  - Sync via timeline semaphores
  - Off by default; the project's Frame graph settings turn it on
- `.fjgraph` — declarative frame layout, hot-reloadable
  - Parser: `fjgraph_parser`
  - Asset: `fjgraph_asset`
  - Watcher: `fjgraph_watcher`
- Limitations (current)
  - The DAG never reorders: a pass runs at `phase + sub_order`, and the
    declared edges feed culling and barriers only
  - The RT reflection output is still transitioned inside its pass
- Order worth knowing
  - Water: `water_depth_seed` (Color 20), `water_depth_prepass` (21), the
    fog volume `fog_medium`..`fog_integrate` (40–43), `water` (45), then
    `transparent` (50); `fog_composite` stays at PostProcess 104. A graph
    that draws water lists the seed, the prepass and the water.
  - SS-GI's `ss_gi_depth_history` and SSR's `ssr_history` copy their
    histories as nodes of their own; a graph listing SS-GI or SSR lists them
    too.
- Roadmap — fine-grained per-dispatch nodes (Phase 3+)
