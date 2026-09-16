# fjell::Actor

Base class for non-component game objects with a transform. Rarely used — prefer [`ScriptComponent`](ScriptComponent.md) for most gameplay code.

| | |
|---|---|
| Header | `fjell/actor.hpp` |
| Source | (header-only base; engine wiring lives in `ScriptManager`) |

## Syntax

```cpp
namespace fjell {

class Actor {
public:
    virtual ~Actor() = default;

    virtual void begin_play();
    virtual void tick(float dt);
    virtual void end_play();

    [[nodiscard]] uint32_t       id()        const;
    [[nodiscard]] const char*    name()      const;
    [[nodiscard]] World*         world();
    [[nodiscard]] const World*   world()    const;
    [[nodiscard]] Transform&     transform();
    [[nodiscard]] const Transform& transform() const;

    // Engine-set fields (treat as read-only)
    uint32_t    id_{0};
    int32_t     parent_id_{-1};
    const char* name_{""};
};

}
```

## Remarks

`Actor` represents a stand-alone object with its own transform that isn't attached to a `SceneNode`'s component list. In practice, most gameplay code lives in [`ScriptComponent`](ScriptComponent.md) subclasses — pick `Actor` only when you specifically need an entity *without* the component-per-node model.

The API surface is a strict subset of `ScriptComponent`:
- Same `begin_play` / `tick` / `end_play` lifecycle.
- Has `transform()` and `world()`, but **no** `input()` / `physics()` / `delta_time()` / `elapsed_time()` convenience accessors — go through `world()->input()`, `world()->delta_time`, etc.
- No collision hooks (`on_collision_enter` etc.) — not attached to a collider.
- No `[[fj::property]]` reflection / inspector integration in the current SDK.

If you find yourself wanting Actor + components + collision + inspector fields, you actually want a `ScriptComponent` on a node.

## Lifecycle functions

| Name | Description |
|---|---|
| `begin_play` | One-time init |
| `tick(float dt)` | Per-frame update with scaled gameplay dt |
| `end_play` | One-time teardown |

## Accessors

| Name | Description |
|---|---|
| `id` | Engine-assigned ID |
| `name` | Display name |
| `transform` | Position / rotation / scale |
| `world` | Per-frame state |

## See also
- [`ScriptComponent`](ScriptComponent.md) — the usual choice
- [`World`](World.md)
- [`Transform`](Transform.md)
