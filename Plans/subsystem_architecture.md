# Subsystem Architecture

## Problem

Every gameplay system (scripting, physics, audio, VFX, animation) was manually orchestrated in `Engine::run()` with ad-hoc parameter lists. Adding a new system meant editing the main loop, growing function signatures, and remembering call order. There was no shared contract for how a system plugs into the engine.

## Philosophy

A **subsystem** is any system that ticks per-frame and/or has play-mode lifecycle hooks. The engine owns all subsystems and drives them through a uniform interface. Subsystems don't know about each other — they receive what they need through a `TickContext` and expose their internals via a typed getter for the few places where direct cross-system access is needed (e.g. collision dispatch between scripts and physics).

**What is NOT a subsystem:**
- Core primitives (Window, Input, Clock, ThreadPool)
- Rendering infrastructure (VulkanContext, ResourceManager, AssetLoader, ParticleSystem)
- Data structures (Scene)
- Editor UI (ImGui panels, ContentBrowser)
- Debug tools (ShaderWatcher)

These are either always-on infrastructure or don't follow the tick/lifecycle pattern.

## Naming convention

One rule: **`XxxSubsystem`**. Always. No "Manager", no "Engine", no "System" suffix for anything that inherits `Subsystem`.

| Subsystem | Wraps | File |
|-----------|-------|------|
| `AnimationSubsystem` | `AnimationSystem` | `src/animation/animation_subsystem.hpp` |
| `AudioSubsystem` | `AudioEngine` | `src/audio/audio_subsystem.hpp` |
| `ScriptSubsystem` | `ScriptManager` | `src/scripting/script_subsystem.hpp` |
| `PhysicsSubsystem` | `PhysicsWorld` | `src/physics/physics_subsystem.hpp` |
| `VfxSubsystem` | (self-contained) | `src/renderer/vfx_subsystem.hpp` |

Each subsystem lives in its domain directory, not in `core/`. The base class and context live in `core/`:
- `src/core/subsystem.hpp` — `Subsystem` base class
- `src/core/frame_context.hpp` — `TickContext` struct

## Base class

```cpp
enum class TickPolicy : uint8_t { ALWAYS, PLAY_ONLY };

class Subsystem {
public:
    virtual ~Subsystem() = default;
    virtual std::string_view name() const = 0;
    virtual TickPolicy tick_policy() const { return TickPolicy::ALWAYS; }
    virtual void tick(const TickContext& ctx) {}
    virtual void on_play_start(const TickContext& ctx) {}
    virtual void on_play_stop(const TickContext& ctx) {}
};
```

No `init()`/`shutdown()` — RAII handles that. No priority numbers — explicit phase groups handle ordering.

## TickContext

```cpp
struct TickContext {
    Scene&         scene;
    const Input&   input;
    const Clock&   clock;
    float          dt;
    ThreadPool&    thread_pool;
    PhysicsWorld*  physics{nullptr};   // null outside play
    AudioEngine*   audio{nullptr};
    VfxSubsystem*  vfx{nullptr};
    const Project* project{nullptr};
};
```

Replaces the ever-growing parameter lists. Named `TickContext` (not `FrameContext`) to avoid collision with the renderer's existing `FrameContext` in `render_pass.hpp`.

## How Engine owns subsystems

```cpp
// Owns all subsystems
std::vector<std::unique_ptr<Subsystem>> subsystems_;

// Typed access (non-owning pointers into the vector)
AnimationSubsystem* animation_subsystem_{};
AudioSubsystem*     audio_subsystem_{};
ScriptSubsystem*    script_subsystem_{};
PhysicsSubsystem*   physics_subsystem_{};
VfxSubsystem*       vfx_subsystem_{};
```

Registered via `add_subsystem<T>(args...)` in the constructor. Each subsystem is both iterable (through phase groups) and directly accessible (through the typed pointer).

## Phase-based main loop

The main loop can't just iterate all subsystems linearly — there are mandatory `flush_pending()` barriers between scripts, physics, and VFX. Instead, subsystems are grouped into phases:

```
play_phase_0_  → ScriptSubsystem       (scripts tick)
               → flush_pending()
play_phase_1_  → PhysicsSubsystem      (physics step + collision dispatch)
               → flush_pending()
play_phase_2_  → VfxSubsystem          (particle lifetime cleanup)

early_always_  → AnimationSubsystem    (before compute_world_matrices)
               → scene_.compute_world_matrices()
late_always_   → AudioSubsystem        (after world matrices, needs camera position)
```

Adding a new subsystem = register it in the right phase. No editing the loop body.

## Play lifecycle

`start_play()` and `stop_play()` remain explicit (not generic iteration) because:
1. Ordering matters — physics must exist before scripts can reference it
2. The `TickContext` must be rebuilt after physics/vfx are created
3. Script serialization/deserialization is engine-level, not subsystem-level

```cpp
void Engine::start_play() {
    script_subsystem_->manager().serialize_all(scene_);
    play_session_ = std::make_unique<PlaySession>(scene_);  // snapshot

    physics_subsystem_->on_play_start(ctx);   // creates PhysicsWorld
    vfx_subsystem_->on_play_start(ctx);
    // rebuild ctx — now has physics pointer
    audio_subsystem_->on_play_start(ctx);     // starts play_on_start sources
    script_subsystem_->on_play_start(ctx);    // prepare + deserialize + begin_play
}
```

## flush_pending

Stays as explicit code in Engine. It's a barrier between phases that touches ScriptManager and PhysicsWorld by name — a specific engine contract, not something to genericize.

```cpp
void Engine::flush_pending() {
    if (scene_.pending_destroy().empty()) return;
    script_subsystem_->manager().trace_doomed_refs(scene_, scene_.pending_destroy());
    if (physics_subsystem_->world()) {
        for (uint32_t id : scene_.pending_destroy())
            physics_subsystem_->world()->remove_body(id);
    }
    scene_.flush_destroyed();
}
```

## PlaySession

Shrunk to just scene snapshot/restore. No longer owns PhysicsWorld — that moved to `PhysicsSubsystem`.

## How to add a new subsystem

1. Create `src/<domain>/foo_subsystem.hpp` inheriting `Subsystem`
2. Implement `name()`, `tick_policy()`, `tick()`, and optionally `on_play_start()`/`on_play_stop()`
3. If it has a `.cpp`, add it to `src/CMakeLists.txt`
4. In `Engine` constructor: `foo_subsystem_ = add_subsystem<FooSubsystem>(args...);`
5. Add the pointer to the right phase group (`play_phase_N_`, `early_always_`, or `late_always_`)
6. If other subsystems need access, add a pointer to `TickContext`
