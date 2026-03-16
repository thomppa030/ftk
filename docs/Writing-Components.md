# Writing Components

## Minimal Example

```cpp
#include "fjell_api.h"

struct Spinner {
    float speed = 90.0f;  // degrees per second
};

static void spinner_update(void* data, const FjellEngineApi* api, float dt) {
    auto* self = static_cast<Spinner*>(data);

    float rot[3];
    api->get_rotation(api->object_id, rot);
    rot[1] += self->speed * dt;
    api->set_rotation(api->object_id, rot);
}

static const FjellFieldDesc spinner_fields[] = {
    FJELL_FIELD_RANGE(Spinner, speed, FJELL_FIELD_FLOAT, 0.0f, 360.0f, 1.0f),
};

static const FjellComponentType spinner_type = {
    .name = "Spinner",
    .instance_size = sizeof(Spinner),
    .version = 1,
    .create = nullptr,
    .destroy = nullptr,
    .update = spinner_update,
    .inspector = nullptr,      // auto-generated from fields
    .serialize = nullptr,      // auto-generated from fields
    .deserialize = nullptr,    // auto-generated from fields
    .fields = spinner_fields,
    .field_count = 1,
};

FJELL_EXPORT_COMPONENTS(&spinner_type)
```

This gives you a component with:
- A `speed` field editable in the inspector (drag float, 0-360, step 1)
- An update function that rotates the object around Y each frame
- Automatic serialization to/from JSON

## Multiple Components

```cpp
FJELL_EXPORT_COMPONENTS(&spinner_type, &player_type, &health_type)
```

All types are registered in a single call. Each object can have one instance of each type.

## Component Data

Component data is a flat struct. The engine allocates `instance_size` bytes, zero-initializes it, and passes a pointer to your callbacks. You cast it to your struct type.

Keep component structs POD-like. Don't use std::string, std::vector, or other types with non-trivial constructors — the engine copies raw bytes for serialization. Use fixed-size arrays and C strings (char buffers) instead.

## Callbacks

All callbacks are optional (set to `nullptr` to skip):

### create

```cpp
void my_create(void* data, const FjellEngineApi* api);
```

Called when a component is instantiated (add in editor, or deserialize on load). Use it to set default values that depend on the object or engine state.

### destroy

```cpp
void my_destroy(void* data);
```

Called when the component is removed or the scene is unloaded. Free any external resources here.

### update

```cpp
void my_update(void* data, const FjellEngineApi* api, float dt);
```

Called every frame during play mode. This is where game logic goes. The `api` provides access to transforms, input, time, logging, and physics.

### inspector

```cpp
void my_inspector(void* data);
```

Custom ImGui UI for the properties panel. If null, the engine generates a UI from the `fields` array. You can use any ImGui call here — the engine includes Dear ImGui headers.

### serialize / deserialize

```cpp
void my_serialize(const void* data, FjellJsonWriter* writer);
void my_deserialize(void* data, const FjellJsonReader* reader);
```

Custom JSON serialization. If null, auto-serialized from fields. Use the `fjell_json_write_*` / `fjell_json_read_*` functions:

```cpp
static void my_serialize(const void* data, FjellJsonWriter* w) {
    auto* self = static_cast<const MyComp*>(data);
    fjell_json_write_float(w, "health", self->health);
    fjell_json_write_vec3(w, "target", &self->target.x);
}

static void my_deserialize(void* data, const FjellJsonReader* r) {
    auto* self = static_cast<MyComp*>(data);
    self->health = fjell_json_read_float(r, "health", 100.0f);
    float fallback[] = {0, 0, 0};
    fjell_json_read_vec3(r, "target", &self->target.x, fallback);
}
```

## Field Descriptors

Use the helper macros:

```cpp
// Basic field (no range constraints)
FJELL_FIELD(MyStruct, my_float, FJELL_FIELD_FLOAT)

// Field with min/max/drag speed
FJELL_FIELD_RANGE(MyStruct, health, FJELL_FIELD_FLOAT, 0.0f, 100.0f, 0.5f)
```

For enum fields, provide labels:

```cpp
static const char* const mode_labels[] = {"Idle", "Walk", "Run"};

FjellFieldDesc{
    .name = "mode",
    .type = FJELL_FIELD_ENUM,
    .offset = offsetof(MyStruct, mode),
    .size = 3,  // number of labels
    .enum_labels = mode_labels,
};
```

## Physics Example

```cpp
static void ball_update(void* data, const FjellEngineApi* api, float dt) {
    (void)data;
    (void)dt;

    // Apply upward impulse on space press
    if (api->is_key_pressed(32)) {  // GLFW_KEY_SPACE = 32
        float impulse[] = {0.0f, 10.0f, 0.0f};
        api->apply_impulse(api->object_id, impulse);
    }

    // Raycast downward to check if grounded
    float origin[3], dir[] = {0, -1, 0};
    api->get_position(api->object_id, origin);
    float hit_pos[3], hit_normal[3], hit_dist;
    uint32_t hit_id;
    if (api->raycast(origin, dir, 1.1f, hit_pos, hit_normal, &hit_dist, &hit_id)) {
        // grounded
    }
}
```

## Build Setup

In your project's `scripts/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(game)

add_library(game SHARED player.cpp)
target_include_directories(game PRIVATE ${CMAKE_SOURCE_DIR}/../../build/scripting)
set_target_properties(game PROPERTIES PREFIX "")  # output: game.so not libgame.so
```

Build with:
```bash
cd my_project/scripts
cmake -B build
cmake --build build
cp build/game.so ..
```

The engine watches for changes to `game.so` in the project root and hot-reloads automatically in debug builds.
