# fjell::Transform

Position / rotation / scale value type. Plain data — not a class with a vtable.

| | |
|---|---|
| Header | `fjell/transform.hpp` |
| Source | (header-only) |

## Syntax

```cpp
namespace fjell {

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // identity
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::vec3 euler_degrees() const;
    void                    set_euler_degrees(const glm::vec3& degrees);
    [[nodiscard]] glm::mat4 matrix() const;
};

[[nodiscard]] Transform decompose_matrix(const glm::mat4& m);

}
```

## Remarks

`Transform` is the value type behind every scene node's transform. You'll see references to it from [`ScriptComponent::transform()`](ScriptComponent.md#scriptcomponenttransform) and [`Actor::transform()`](Actor.md). Reading and writing fields directly is the expected interface.

Rotation is a quaternion (`glm::quat`, stored as `w, x, y, z`). The euler helpers exist for inspector / serialization use — they're convenient when you have angles in degrees but you should usually compose rotations with `glm::angleAxis` or `glm::quat_cast` for runtime math.

Scale is per-axis. Non-uniform scale is allowed but interacts badly with physics and with skinned mesh transforms — keep components equal where possible.

The shared include guard `FJELL_TRANSFORM_HPP` is intentional: both `fjell/transform.hpp` (SDK side) and `scene/transform.hpp` (engine side) define the same struct, and the guard prevents an ODR conflict when game code includes both transitively.

## Members

| Name | Type | Description |
|---|---|---|
| `position` | `glm::vec3` | World position. Defaults to origin. |
| `rotation` | `glm::quat` | Rotation as a unit quaternion. Defaults to identity. |
| `scale` | `glm::vec3` | Per-axis scale. Defaults to `{1, 1, 1}`. |

## Functions

| Name | Description |
|---|---|
| [`euler_degrees`](#transformeuler_degrees) | Read rotation as XYZ Euler angles in degrees |
| [`set_euler_degrees`](#transformset_euler_degrees) | Write rotation from XYZ Euler angles in degrees |
| [`matrix`](#transformmatrix) | Build the TRS 4×4 matrix |

## Free functions

| Name | Description |
|---|---|
| [`decompose_matrix`](#decompose_matrix) | Recover a `Transform` from a 4×4 TRS matrix |

---

## Transform::euler_degrees

### Syntax
```cpp
[[nodiscard]] glm::vec3 euler_degrees() const;
```

### Return value
The rotation converted to XYZ Euler angles in degrees, via `glm::eulerAngles`.

### Remarks
Convenient for UI / serialization. Euler representations have gimbal-lock pathologies — round-tripping a quaternion through Euler and back can lose information. Don't use this in tight runtime math loops.

---

## Transform::set_euler_degrees

### Syntax
```cpp
void set_euler_degrees(const glm::vec3& degrees);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `degrees` | `glm::vec3` | XYZ Euler angles in degrees. |

### Remarks
Replaces `rotation` with `glm::quat(glm::radians(degrees))`. Order follows GLM's convention for `glm::quat(vec3)`.

---

## Transform::matrix

### Syntax
```cpp
[[nodiscard]] glm::mat4 matrix() const;
```

### Return value
The 4×4 transform matrix in column-major order:
```
T * R * S
```
where `T = translate(position)`, `R = mat4_cast(rotation)`, `S = scale(scale)`.

### Remarks
Compute once and cache when you need it multiple times — the function rebuilds from scratch on every call.

---

## decompose_matrix

### Syntax
```cpp
[[nodiscard]] Transform decompose_matrix(const glm::mat4& m);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `m` | `const glm::mat4&` | A TRS matrix produced by `position × rotation × scale` composition. |

### Return value
A `Transform` whose `position`, `rotation`, `scale` recompose to (approximately) the same matrix.

### Remarks
- Translation comes from the 4th column.
- Scale is the length of each rotation column.
- Rotation is normalized columns cast to quaternion.

Doesn't handle shear or non-uniform mirror correctly — assumes the matrix is well-formed TRS.

## Example

```cpp
// Compose: spin then translate
transform().position += transform().rotation * glm::vec3{0, 0, -1} * dt * speed;

// Read for serialization
glm::vec3 e = transform().euler_degrees();
fjell::Log::info_f("rot = %.1f %.1f %.1f", e.x, e.y, e.z);

// Build a matrix for a shader uniform
glm::mat4 m = transform().matrix();
```

## See also
- [`Node3D::position`](Node3D.md#node3dposition) / `rotation` / `scale` — same data, accessed by handle
- [`ScriptComponent::transform`](ScriptComponent.md#scriptcomponenttransform)
