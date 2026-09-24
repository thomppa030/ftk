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
  - RT pass managed manually (compute + RT same stage in graph)
  - DDGI/SS-GI manage their own layouts
- Roadmap — fine-grained per-dispatch nodes (Phase 3+)
