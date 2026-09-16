# Asset Pipeline

> Stub — fill in.

## Scope
How assets get from source files into a running game: import, baking, async load, hot-reload.

## Planned sections
- Philosophy
  - Authored data stays text/JSON in dev (FJMAT, FJUI, FJSS, FJSCENE, FJVFX, FJACTION, FJINPUT, FJGRAPH, FJWIDGET)
  - Heavy data goes binary (FJMESH, FJSKEL, FJANIM)
  - Self-contained `.fj*` formats — never depend on third-party library formats
- Importers
  - `fjimport` CLI — `.glb`/`.gltf`/`.fbx` → `.fjmesh`/`.fjskel`/`.fjanim`
  - In-engine import dialog (`src/import/`)
- Runtime loading
  - Sync vs async
  - `asset_loader` — background CPU file I/O + meshlet build, sync GPU upload on main thread
  - `LoadState` enum
- Caching — `.fjcache/` (thumbnails, baked data)
- UUIDs — `AssetId`, stable across renames
- `ResourceManager` & handles — `Handle<Tag>` generational refs
- Hot-reload paths
  - Shaders / FJSL: `shader_watcher`
  - FJUI / FJSS: `ui_watcher`
  - FJGRAPH: `fjgraph_watcher`
  - Scripts: build & reload from editor
- Adding a new asset type — registry, type metadata, inspector hook
