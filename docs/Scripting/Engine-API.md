# Engine API Index

Reference pages for every type and static API exposed to game scripts. For the mental model, see [Overview](Overview.md).

## Static APIs (`*_api.hpp`)

| API | Header | Description |
|---|---|---|
| [`Log`](API/Log.md) | `fjell/log_api.hpp` | Log messages to the editor console |
| [`Time`](API/Time.md) | `fjell/time_api.hpp` | Scale or pause gameplay time |
| [`Timer`](API/Timer.md) | `fjell/timer_api.hpp` | Schedule one-shot and repeating callbacks |
| [`scene`](API/scene_api.md) | `fjell/scene_api.hpp` | Find, spawn, and destroy scene nodes |
| [`camera`](API/camera_api.md) | `fjell/camera_api.hpp` | Active camera and projection control |
| [`material`](API/material_api.md) | `fjell/material_api.hpp` | Override one node's material parameters |
| [`physics`](../Physics.md#script-api) | `fjell/physics_api.hpp` | Raycasts, shape casts, overlaps and gravity |
| [`input`](API/input_api.md) | `fjell/input_api.hpp` | Bind input actions to member functions |
| [`audio`](API/audio_api.md) | `fjell/audio_api.hpp` | Play audio clips at world positions |
| [`vfx`](API/vfx_api.md) | `fjell/vfx_api.hpp` | Spawn particle systems |
| [`Debug`](API/Debug.md) | `fjell/debug_api.hpp` | Debug-draw lines, boxes, spheres, points; on-screen debug text |
| [`viewport`](API/viewport_api.md) | `fjell/viewport_api.hpp` | Create render-to-texture viewports |

## Base classes

| Class | Header | Description |
|---|---|---|
| [`ScriptComponent`](API/ScriptComponent.md) | `fjell/component.hpp` | Base for components attached to scene nodes |
| [`UiWidget`](API/UiWidget.md) | `fjell/ui_widget.hpp` | Base for `.fjwidget`-bound UI controllers |
| [`AnimNotify`](API/AnimNotify.md) | `fjell/notify.hpp` | Base for instant animation notifies |
| [`AnimNotifyState`](API/AnimNotify.md#animnotifystate) | `fjell/notify.hpp` | Base for span-style animation notifies |

## Engine components

Engine components a script reaches directly, through a `Ref<T>` or `get_component<T>()`.
The ones whose fields a script reads are checked when the game library loads, so a library
built against another engine is refused rather than misread; `AnimatorComponent` and
`WaterComponent` have no fields a script sees, only methods.

| Component | Header | What a script does with it |
|---|---|---|
| `RigidbodyComponent` | `fjell/engine_components.hpp` | Set up a body; `add_force`, `add_impulse`, read and set its velocity ([Physics](../Physics.md#script-api)) |
| `ColliderComponent` | `fjell/engine_components.hpp` | Shape, channel and trigger of a collider |
| `CharacterComponent` | `fjell/engine_components.hpp` | Move a character by intent or velocity, `jump()`, read its ground |
| `MeshComponent` | `fjell/engine_components.hpp` | Read which model and material a mesh draws with |
| `SpriteComponent` | `fjell/engine_components.hpp` | Tint, flip (`flip_h`) and look of a sprite |
| `SpriteAnimComponent` | `fjell/sprite_anim_component.hpp` | Drive a sprite animation: `state_machine.set_param()`, `playing`, `finished` |
| `AnimatorComponent` | `fjell/animator_component.hpp` | Drive a skeletal animation: `state_machine().set_param()`, `current_state_name()` |
| `WaterComponent` | `fjell/water_component.hpp` | Ask a sea what it is doing: `wave_height()`, `bed_agitation(depth)`, and the silt model |

## Handles and value types

| Type | Header | Description |
|---|---|---|
| [`Node3D`](API/Node3D.md) | `fjell/node.hpp` | A node in the scene, as a script sees it |
| [`Transform`](API/Transform.md) | `fjell/transform.hpp` | Position / rotation / scale value type |
| [`World`](API/World.md) | `fjell/world.hpp` | Per-frame state passed to components |
| [`Input`](API/Input.md) | `fjell/input.hpp` | Keyboard / mouse polling |
| [`Asset<T>`](API/Asset.md) | `fjell/asset.hpp` | Handle to an engine asset |
| [`Ref<T>`](API/Ref.md) | `fjell/ref.hpp` | Reference to a component on a node, which knows when it is gone |
| [`Delegate<>`](API/Delegate.md) | `fjell/delegate.hpp` | Multicast event with RAII subscriptions |
| [`Connection`](API/Delegate.md#connection) | `fjell/delegate.hpp` | RAII subscription handle returned by `Delegate::bind` |
| [`TimerHandle`](API/Timer.md#timerhandle) | `fjell/timer_api.hpp` | Opaque handle returned by `Timer::set` |

## Macros

| Macro | Header | Description |
|---|---|---|
| `FJELL_COMPONENT(T)` | `fjell/registry.hpp` | Register a `ScriptComponent` subclass |
| `FJELL_UI_WIDGET(T)` | `fjell/ui_registry.hpp` | Register a `UiWidget` subclass |
| `FJELL_NOTIFY(T)` | `fjell/registry.hpp` | Register an `AnimNotify` subclass |
| `FJELL_REGISTER()` | `fjell/registry.hpp` | Emit the `.so` registration entry point (once per game module) |

## Field annotations

| Annotation | Where | Description |
|---|---|---|
| `[[fj::property]]` | `ScriptComponent` field | Reflected; shown in scene properties panel; serialized to `.fjscene` |
| `[[fj::state]]` | field or `const` getter | Runtime value the game UI reads; shown, never saved |
| `[[fj::model]]` | `UiWidget` struct field | A Model the widget's markup reads by the field's name |
| `[[fj::prop]]` | `UiWidget` field | Filled from the widget's prop of the same name |
| `[[fj::action]]` | `UiWidget` method | Callable from `.fjui` markup via `on:click="name"` |

## Key codes

`fjell/api.h` defines `FJELL_KEY_*` and `FJELL_MOUSE_*` constants, Fjell's own codes for keys by their place on a US keyboard. Used with [`Input`](API/Input.md) polling functions.
