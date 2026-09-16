# Engine Overview

> Stub — fill in.

## Scope
The big picture: what subsystems exist, how the main loop drives them, how the editor wraps it, how scripts hot-reload.

## Planned sections
- Top-level types
  - `Engine` — owns subsystems, window, clock
  - `PlayController` — play lifecycle, owns the `PlaySession` while playing
  - `ProjectManager` — discover/load projects
- Subsystem list (current)
  - `RenderSubsystem` (Vulkan)
  - `AnimationSubsystem` (ozz)
  - `UiSubsystem` (game UI)
  - `ScriptSubsystem` (hot-reloaded game module)
  - Audio, Physics, ...
- Main loop
  - Split-frame design (sim ↔ render pipelining)
  - Frame phases: input → fixed update → variable update → render submit
  - `FrameContext`
- Hot-reload model — script DLL/SO reload, what state survives
- Editor vs play mode
- Threading model — main, render submit, asset loading, thread pool
