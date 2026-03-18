# Scripting Overview

Components are C++ classes compiled into a shared library (`.so`). The engine loads the library at runtime, discovers your types through auto-registration, and calls into them each frame during play mode.

## How It Works

1. Write a component class that extends `fjell::Component`
2. Use the `FJELL_COMPONENT` macro to register it
3. Hit **Scripts > Build & Reload** in the editor
4. Attach the component to any object in the inspector
5. Press Play — `begin_play()` fires once, `tick()` fires every frame

## Component Lifecycle

```
Object enters play mode:
  → begin_play()

Each frame:
  → tick(dt)

Object exits play mode / is destroyed:
  → end_play()
```

All three methods are optional. Override only what you need.

## Object Model

Components live on objects via the Actor system. Every component can reach its owning actor and the engine's World:

```
World (engine services: input, physics, scene, time, logging)
  └── Actor (id, name, transform)
        └── Component (your game logic)
```

From any component:
- `actor()` — the actor this component is attached to
- `actor()->transform()` — read/write position, rotation, scale
- `world()` — access input, physics, scene, timing, and logging

## Exposed Fields and the Inspector

If your component defines a `reflect()` method, the engine auto-generates an inspector UI and handles serialization. Supported field types:

| Type | Inspector Widget |
|------|-----------------|
| `FJELL_FIELD_FLOAT` | DragFloat |
| `FJELL_FIELD_INT` | DragInt |
| `FJELL_FIELD_BOOL` | Checkbox |
| `FJELL_FIELD_VEC2` | DragFloat2 |
| `FJELL_FIELD_VEC3` | DragFloat3 |
| `FJELL_FIELD_VEC4` | DragFloat4 |
| `FJELL_FIELD_COLOR3` | ColorEdit3 |
| `FJELL_FIELD_COLOR4` | ColorEdit4 |
| `FJELL_FIELD_STRING` | InputText |
| `FJELL_FIELD_ENUM` | Combo |

Fields with `FJELL_F_RANGE` get clamped drag widgets with a configurable speed.

## Build & Reload

From the editor menu: **Scripts > Build & Reload**. The engine:

1. Scans `scripts/` for `FJELL_COMPONENT` macros
2. Auto-generates `register_all.cpp`
3. Runs cmake configure + build
4. Hot-reloads the `.so` — serializes all instances, unloads the old library, loads the new one, deserializes back

You see your changes without restarting the editor.

## Creating New Components

**Scripts > New Component** generates a template `.cpp` file in your project's `scripts/` directory with the boilerplate already filled in. Edit it, hit Build & Reload, and you're good to go.

Scripts show up in the Content Browser and can be opened by double-clicking to edit in the embedded neovim panel.

## Hot-Reload (Debug Builds)

The engine watches the `.so` file via inotify. When it changes:

1. Serialize all component instances to JSON
2. Destroy all instances, close old `.so`
3. Load new `.so`
4. Re-register component types
5. Deserialize all instances from JSON

This means you can recompile your game code and see changes without restarting the editor.
