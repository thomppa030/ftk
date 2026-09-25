# Engine API Reference

> **Out of date.** This page describes an earlier script API. The current one
> is catalogued in [Engine API](Scripting/Engine-API.md), with the three ticks
> in [Scripting Overview](Scripting/Overview.md#the-three-ticks).

## Component

Base class for all game components. Defined in `scripting/fjell_component.hpp`.

```cpp
class Component {
public:
    virtual void begin_play() {}
    virtual void tick(float dt) {}
    virtual void end_play() {}

    Actor* actor();
    World* world();
};
```

| Method | Description |
|--------|-------------|
| `begin_play()` | Called once when play mode starts. |
| `tick(dt)` | Called every frame. `dt` is seconds since last frame. |
| `end_play()` | Called when play stops or the object is destroyed. |
| `actor()` | Returns the actor this component is attached to. |
| `world()` | Returns the World for engine services. |

All lifecycle methods are optional overrides.

## Actor

An entity in the scene. Defined in `scripting/fjell_actor.hpp`.

```cpp
class Actor {
public:
    uint32_t id() const;
    const char* name() const;
    World* world();
    Transform& transform();
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `id()` | `uint32_t` | Unique ID in the scene. |
| `name()` | `const char*` | Display name (set in the editor). |
| `world()` | `World*` | Engine services. |
| `transform()` | `Transform&` | Read/write position, rotation, and scale. |

## Transform

Position, rotation, and scale of an object. Defined in `scene/transform.hpp`.

```cpp
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};   // identity
    glm::vec3 scale{1.0f};

    glm::vec3 euler_degrees() const;
    void set_euler_degrees(const glm::vec3& degrees);
    glm::mat4 matrix() const;
};
```

| Member/Method | Description |
|--------|-------------|
| `position` | World-space position. Read/write directly. |
| `rotation` | Orientation as a quaternion. |
| `scale` | Scale on each axis. `{1,1,1}` is default. |
| `euler_degrees()` | Returns rotation as Euler angles in degrees (XYZ). |
| `set_euler_degrees(v)` | Sets rotation from Euler angles in degrees. |
| `matrix()` | Returns the 4x4 TRS model matrix. |

```cpp
auto& t = actor()->transform();
t.position = {10.0f, 0.0f, 5.0f};

auto euler = t.euler_degrees();
euler.y += 90.0f * dt;
t.set_euler_degrees(euler);

// Or use quaternions directly
t.rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3{0, 1, 0});
```

## World

Engine services accessible from any component. Defined in `scripting/fjell_world.hpp`.

```cpp
struct World {
    float delta_time;
    float elapsed_time;

    const Input& input() const;
    PhysicsWorld* physics();
    Scene* scene();

    void log_info(const char* msg) const;
    void log_warn(const char* msg) const;
    void log_error(const char* msg) const;
};
```

| Member/Method | Description |
|--------|-------------|
| `delta_time` | Seconds since last frame (same as `dt` in `tick()`). |
| `elapsed_time` | Total seconds since engine start. |
| `input()` | Keyboard and mouse state. |
| `physics()` | Physics simulation (forces, velocity, raycasting). |
| `scene()` | Scene graph access. |
| `log_info/warn/error` | Log to the editor console (shows with `[script]` prefix). |

## Input

Keyboard and mouse state. Accessed via `world()->input()`.

```cpp
bool is_key_pressed(int key) const;     // true for one frame on key down
bool is_key_held(int key) const;        // true while key is held
bool is_key_released(int key) const;    // true for one frame on key up

bool is_mouse_button_pressed(int button) const;
bool is_mouse_button_held(int button) const;

glm::dvec2 mouse_position() const;
glm::dvec2 mouse_delta() const;
glm::dvec2 scroll_delta() const;
```

Key codes are GLFW constants. Common ones:

| Constant | Value |
|----------|-------|
| `GLFW_KEY_W` | 87 |
| `GLFW_KEY_A` | 65 |
| `GLFW_KEY_S` | 83 |
| `GLFW_KEY_D` | 68 |
| `GLFW_KEY_SPACE` | 32 |
| `GLFW_KEY_LEFT_SHIFT` | 340 |

Mouse buttons: 0 = left, 1 = right, 2 = middle.

During play mode the cursor is captured, so `mouse_delta()` returns relative movement.

## PhysicsWorld

Physics simulation. Accessed via `world()->physics()`.

### Forces and Impulses

```cpp
void apply_force(uint32_t id, const glm::vec3& force);    // continuous, call every frame
void apply_impulse(uint32_t id, const glm::vec3& impulse); // instantaneous velocity change
```

### Velocity

```cpp
void set_linear_velocity(uint32_t id, const glm::vec3& vel);
glm::vec3 get_linear_velocity(uint32_t id) const;
void set_angular_velocity(uint32_t id, const glm::vec3& vel);  // radians/sec
glm::vec3 get_angular_velocity(uint32_t id) const;
```

### Raycasting

```cpp
bool raycast(const glm::vec3& origin, const glm::vec3& direction,
             float max_distance, RaycastHit& out_hit) const;
```

Returns `true` if something was hit. The `RaycastHit` struct:

```cpp
struct RaycastHit {
    glm::vec3 position;     // world-space hit point
    glm::vec3 normal;       // surface normal at hit
    float distance;          // distance from origin
    uint32_t object_id;      // ID of the object that was hit
};
```

All physics methods are null-safe — they no-op outside play mode.

## Field Reflection

Macros for exposing member variables to the inspector and serialization.

```cpp
FJELL_F(Class, member, type)                           // basic field
FJELL_F_RANGE(Class, member, type, min, max, speed)    // with min/max/drag speed
FJELL_F_ENUM(Class, member, labels, count)             // enum combo box
```

### Field Types

| Type constant | C++ type | Inspector widget |
|---------------|----------|-----------------|
| `FJELL_FIELD_FLOAT` | `float` | DragFloat |
| `FJELL_FIELD_INT` | `int32_t` | DragInt |
| `FJELL_FIELD_BOOL` | `bool` | Checkbox |
| `FJELL_FIELD_VEC2` | `glm::vec2` | DragFloat2 |
| `FJELL_FIELD_VEC3` | `glm::vec3` | DragFloat3 |
| `FJELL_FIELD_VEC4` | `glm::vec4` | DragFloat4 |
| `FJELL_FIELD_COLOR3` | `glm::vec3` | ColorEdit3 |
| `FJELL_FIELD_COLOR4` | `glm::vec4` | ColorEdit4 |
| `FJELL_FIELD_STRING` | `char[]` | InputText |
| `FJELL_FIELD_ENUM` | `int` | Combo |

## Registration Macros

| Macro | Purpose |
|-------|---------|
| `FJELL_COMPONENT(ClassName)` | Declares and registers a component type. Put at the top of your class body. |
| `FJELL_REGISTER()` | Defines the `.so` entry point. Goes in `register_all.cpp` (auto-generated by Build & Reload). |
