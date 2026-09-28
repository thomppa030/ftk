# fjell::material

Overrides shader parameters on one node's material, without touching the others that
share it.

| | |
|---|---|
| Header | `fjell/material_api.hpp` |
| Source | `src/scripting/material_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace material {
    void set_param(const Ref<Node3D>& node, const char* name, float v);
    void set_param(const Ref<Node3D>& node, const char* name, float x, float y);
    void set_param(const Ref<Node3D>& node, const char* name, float x, float y, float z);
    void set_param(const Ref<Node3D>& node, const char* name, float x, float y, float z, float w);
    void reset(const Ref<Node3D>& node);
}
}
```

## Remarks

A node draws with the material of its mesh, or of its body of water; these calls change
whichever it has. `self()` passes the script's own node, and any other node is a
[`Ref<Node3D>`](Ref.md), best a `[[fj::property]]` picked in the inspector. A node that is
gone stops the game.

Nodes start out sharing a base material. The first `set_param()` on a node clones that
base into a private instance for the node, and later calls change the clone. The clone
costs memory and a pipeline lookup, so edit the `.fjmat` instead when every user of a
material should change together.

Parameter names are the `uniform` names in the material's `.fjsl` shader. An unknown name
is ignored, as is a node that draws with no material: a typo shows up as a parameter that
never changes.

## Functions

| Name | Description |
|---|---|
| [`set_param`](#materialset_param) | Set a `float`, `vec2`, `vec3` or `vec4` uniform on the node's material |
| [`reset`](#materialreset) | Return the node to the shared material |

---

## material::set_param

```cpp
void set_param(const Ref<Node3D>& node, const char* name, float v);
void set_param(const Ref<Node3D>& node, const char* name, float x, float y);
void set_param(const Ref<Node3D>& node, const char* name, float x, float y, float z);
void set_param(const Ref<Node3D>& node, const char* name, float x, float y, float z, float w);
```

| Name | Type | Description |
|---|---|---|
| `node` | `const Ref<Node3D>&` | The node whose mesh or water to change. |
| `name` | `const char*` | Uniform name from the material's `.fjsl`. |
| `v`, `x`..`w` | `float` | The value, one float per component of the uniform. |

A `vec3` is also the form for a `hint_color` colour, as sRGB exactly as the picker and the
`.fjmat` show it; the engine converts it to linear on the way to the GPU. A `vec4` is the
form for an RGBA colour.

---

## material::reset

```cpp
void reset(const Ref<Node3D>& node);
```

Discards the node's private material instance and returns it to the shared base material,
dropping every override at once. Safe on a node that was never overridden.

## Example

Flash a hit enemy red, then restore it:

```cpp
void Enemy::take_hit() {
    fjell::material::set_param(self(), "tint", 1.0f, 0.2f, 0.2f);
    fjell::Timer::set<&Enemy::clear_flash>(0.1f, this);
}

void Enemy::clear_flash() { fjell::material::reset(self()); }
```

## See also

- [`Ref`](Ref.md) — referring to another node
- [Materials](../../Materials.md) — `.fjsl` shaders and `.fjmat` files
