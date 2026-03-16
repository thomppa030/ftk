# Scripting Overview

Components are written in C++ and compiled to a shared library (`.so`). The engine loads the library at runtime via `dlopen`, discovers component types through a registration function, and calls into them each frame.

## How It Works

```
1. Write component code (C++ with fjell_api.h)
2. Compile to game.so
3. Engine loads game.so, calls fjell_register_components()
4. Engine gets back an array of FjellComponentType descriptors
5. Components can be added to objects in the inspector
6. Each frame, Update callbacks are called with FjellEngineApi
7. On save/load, component data is serialized to/from JSON
```

## Component Lifecycle

```
add_component()
  → allocate data blob (instance_size bytes, zero-initialized)
  → call create(data, api)           [if provided]

each frame (play mode):
  → call update(data, api, dt)       [if provided]

serialize (play start, scene save):
  → call serialize(data, writer)     [or auto-serialize from fields]

deserialize (play stop, scene load):
  → allocate fresh data blob
  → call create(data, api)
  → call deserialize(data, reader)   [or auto-deserialize from fields]

remove_component():
  → call destroy(data)               [if provided]
  → free data blob
```

## Hot-Reload (Debug Builds)

The engine watches the `.so` file via inotify. When it changes:

1. Serialize all component instances to JSON
2. Destroy all instances, close old `.so`
3. Load new `.so` (copied to temp path to avoid dlopen caching)
4. Re-register component types
5. Deserialize all instances from JSON

This means you can recompile your game code and see changes without restarting the editor.

## Auto-Serialization and Auto-Inspector

If a component type doesn't provide custom `serialize`/`deserialize`/`inspector` callbacks, the engine generates them automatically from the `fields` array in `FjellComponentType`. Supported field types:

| Type | Inspector Widget | JSON Format |
|------|-----------------|-------------|
| `FJELL_FIELD_FLOAT` | DragFloat | number |
| `FJELL_FIELD_INT` | DragInt | number |
| `FJELL_FIELD_BOOL` | Checkbox | bool |
| `FJELL_FIELD_VEC2` | DragFloat2 | [x, y] |
| `FJELL_FIELD_VEC3` | DragFloat3 | [x, y, z] |
| `FJELL_FIELD_VEC4` | DragFloat4 | [x, y, z, w] |
| `FJELL_FIELD_COLOR3` | ColorEdit3 | [r, g, b] |
| `FJELL_FIELD_COLOR4` | ColorEdit4 | [r, g, b, a] |
| `FJELL_FIELD_STRING` | InputText | string |
| `FJELL_FIELD_ENUM` | Combo | int |

Fields can specify `min`, `max`, and `speed` for drag widgets.

## Project Script Setup

A typical project has:

```
scripts/
  CMakeLists.txt
  player.cpp
```

The `CMakeLists.txt` compiles to `game.so` and includes `fjell_api.h` from the engine's build directory.
