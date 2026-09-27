# Writing Components

> **Out of date.** This page was written for an earlier script API (`actor()`,
> `reflect()`, `world()->physics()`) and a simulation that stepped once a frame.
> For how scripts work now — `tick`, `fixed_tick`, `late_tick`, `PhysicsAPI` —
> see [Scripting Overview](Scripting/Overview.md) and [Physics](Physics.md).

## Minimal Example

```cpp
#include "scripting/fjell_registry.hpp"
#include "scripting/fjell_world.hpp"

class Spinner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Spinner)

    float speed = 90.0f;

    static auto reflect() {
        return std::array{
            FJELL_F_RANGE(Spinner, speed, FJELL_FIELD_FLOAT, 0, 360, 1),
        };
    }

    void tick(float dt) override {
        auto& t = actor()->transform();
        auto euler = t.euler_degrees();
        euler.y += speed * dt;
        t.set_euler_degrees(euler);
    }
};
```

That's a complete component. `FJELL_COMPONENT` handles registration. `reflect()` exposes `speed` to the inspector. `tick()` runs every frame.

## The Three Lifecycle Methods

All optional — override only what you need:

```cpp
void begin_play() override;   // called once when play starts
void tick(float dt) override;  // called every frame (dt = seconds since last frame)
void end_play() override;      // called when play stops or object is destroyed
```

## Accessing the Actor and Transform

Every component has a pointer to its owning actor:

```cpp
void tick(float dt) override {
    auto& t = actor()->transform();

    // Position
    t.position.z += 5.0f * dt;

    // Rotation via Euler angles (degrees)
    auto euler = t.euler_degrees();
    euler.y += 90.0f * dt;
    t.set_euler_degrees(euler);

    // Rotation via quaternion (for advanced use)
    t.rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3{0, 1, 0});

    // Scale
    t.scale = glm::vec3(2.0f);

    // Actor metadata
    uint32_t id = actor()->id();
    const char* name = actor()->name();
}
```

The `Transform` stores rotation as a quaternion internally. Use `euler_degrees()` / `set_euler_degrees()` for the common case.

## Reading Input

```cpp
void tick(float dt) override {
    const auto& input = world()->input();

    if (input.is_key_held(FJELL_KEY_W)) {
        actor()->transform().position.z -= move_speed * dt;
    }
    if (input.is_key_pressed(FJELL_KEY_SPACE)) {
        jump();
    }

    // Mouse look
    glm::dvec2 delta = input.mouse_delta();
    auto euler = actor()->transform().euler_degrees();
    euler.y += static_cast<float>(delta.x) * look_sensitivity;
    actor()->transform().set_euler_degrees(euler);

    // Mouse buttons: 0 = left, 1 = right, 2 = middle
    if (input.is_mouse_button_pressed(0)) {
        shoot();
    }
}
```

Key codes are the `FJELL_KEY_*` constants from `fjell/api.h` (`FJELL_KEY_W`, `FJELL_KEY_SPACE`, etc.).

## Using Physics

```cpp
void tick(float dt) override {
    auto* phys = world()->physics();

    // Jump on space press
    if (world()->input().is_key_pressed(FJELL_KEY_SPACE)) {
        phys->apply_impulse(actor()->id(), {0.0f, 10.0f, 0.0f});
    }

    // Continuous thrust
    phys->apply_force(actor()->id(), {0.0f, 0.0f, -50.0f});

    // Direct velocity control
    phys->set_linear_velocity(actor()->id(), {0.0f, 5.0f, 0.0f});

    // Raycast
    fjell::RaycastHit hit;
    glm::vec3 origin = actor()->transform().position;
    if (phys->raycast(origin, {0, -1, 0}, 1.1f, hit)) {
        world()->log_info("grounded");
    }
}
```

Physics calls no-op silently if the physics world isn't active.

## Logging

```cpp
void begin_play() override {
    world()->log_info("Player spawned");
    world()->log_warn("Health is low");
    world()->log_error("Something went wrong");
}
```

Messages appear in the editor console and stdout, prefixed with `[script]`.

## Exposing Fields to the Inspector

Define a static `reflect()` method that returns an array of field descriptors:

```cpp
class PlayerController : public fjell::ScriptComponent {
    FJELL_COMPONENT(PlayerController)

    float move_speed = 5.0f;
    float jump_force = 10.0f;
    int max_health = 100;
    bool god_mode = false;
    glm::vec3 spawn_point{0.0f, 5.0f, 0.0f};

    static auto reflect() {
        return std::array{
            FJELL_F_RANGE(PlayerController, move_speed, FJELL_FIELD_FLOAT, 0, 50, 0.5),
            FJELL_F_RANGE(PlayerController, jump_force, FJELL_FIELD_FLOAT, 0, 100, 0.5),
            FJELL_F_RANGE(PlayerController, max_health, FJELL_FIELD_INT, 1, 999, 1),
            FJELL_F(PlayerController, god_mode, FJELL_FIELD_BOOL),
            FJELL_F(PlayerController, spawn_point, FJELL_FIELD_VEC3),
        };
    }
};
```

### Field Macros

```cpp
FJELL_F(Class, member, type)                           // basic field
FJELL_F_RANGE(Class, member, type, min, max, speed)    // constrained with drag speed
FJELL_F_ENUM(Class, member, labels, count)             // enum combo box
```

### Enum Fields

```cpp
static constexpr const char* const mode_labels[] = {"Idle", "Walk", "Run"};
int mode = 0;

static auto reflect() {
    return std::array{
        FJELL_F_ENUM(MyComponent, mode, mode_labels, 3),
    };
}
```

## Multiple Components in One File

Each just needs its own `FJELL_COMPONENT` macro:

```cpp
class Health : public fjell::ScriptComponent {
    FJELL_COMPONENT(Health)
    float hp = 100.0f;
    // ...
};

class DamageFlash : public fjell::ScriptComponent {
    FJELL_COMPONENT(DamageFlash)
    // ...
};
```

## Registration

The `FJELL_COMPONENT` macro handles auto-registration through a static linked list. You don't need to manually register anything.

Your project needs one `register_all.cpp` file (auto-generated by Build & Reload):

```cpp
#include "scripting/fjell_registry.hpp"
FJELL_REGISTER()
```

## Full Example: First-Person Controller

```cpp
#include "scripting/fjell_registry.hpp"
#include "scripting/fjell_world.hpp"

class FPSController : public fjell::ScriptComponent {
    FJELL_COMPONENT(FPSController)

    float move_speed = 8.0f;
    float look_sensitivity = 0.15f;
    float jump_force = 8.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;

    static auto reflect() {
        return std::array{
            FJELL_F_RANGE(FPSController, move_speed, FJELL_FIELD_FLOAT, 0, 50, 0.5),
            FJELL_F_RANGE(FPSController, look_sensitivity, FJELL_FIELD_FLOAT, 0.01, 1, 0.01),
            FJELL_F_RANGE(FPSController, jump_force, FJELL_FIELD_FLOAT, 0, 50, 0.5),
        };
    }

    void tick(float dt) override {
        const auto& input = world()->input();
        auto& t = actor()->transform();

        // Mouse look
        auto mouse = input.mouse_delta();
        yaw -= static_cast<float>(mouse.x) * look_sensitivity;
        pitch -= static_cast<float>(mouse.y) * look_sensitivity;
        pitch = glm::clamp(pitch, -89.0f, 89.0f);
        t.set_euler_degrees({pitch, yaw, 0.0f});

        // Movement (WASD)
        glm::vec3 forward = glm::normalize(glm::vec3{
            sin(glm::radians(yaw)), 0.0f, -cos(glm::radians(yaw))
        });
        glm::vec3 right = glm::normalize(glm::cross(forward, {0, 1, 0}));

        glm::vec3 move{0.0f};
        if (input.is_key_held(FJELL_KEY_W)) move += forward;
        if (input.is_key_held(FJELL_KEY_S)) move -= forward;
        if (input.is_key_held(FJELL_KEY_D)) move += right;
        if (input.is_key_held(FJELL_KEY_A)) move -= right;

        if (glm::length(move) > 0.0f) {
            move = glm::normalize(move) * move_speed;
        }

        auto* phys = world()->physics();
        auto vel = phys->get_linear_velocity(actor()->id());
        phys->set_linear_velocity(actor()->id(), {move.x, vel.y, move.z});

        // Jump
        if (input.is_key_pressed(FJELL_KEY_SPACE)) {
            phys->apply_impulse(actor()->id(), {0, jump_force, 0});
        }
    }
};
```
