# Subsystems

> Stub — fill in.

## Scope
The `Subsystem` interface, registration, ordering, and the current set of subsystems running in the engine.

## Planned sections
- The `Subsystem` interface (`src/core/subsystem.hpp`)
  - `OnInit`, `OnShutdown`, `OnFrame` / phase hooks
- Registration + dependency ordering
- Current subsystems
  - `RenderSubsystem` — Vulkan, framegraph execution
  - `AnimationSubsystem` — ozz sampling & blending
  - `UiSubsystem` — game UI tick + draw
  - `ScriptSubsystem` — hot-reloaded game module, lifecycle dispatch
  - Audio subsystem
  - Physics subsystem
- Per-subsystem editor context (right-side tabs) — where the editor surfaces subsystem state
- How to add a new subsystem
