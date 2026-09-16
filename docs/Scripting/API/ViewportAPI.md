# fjell::ViewportAPI

Creates render-to-texture viewports. Renders the scene from any camera into a bindless texture that materials can sample via `$viewport:name`.

| | |
|---|---|
| Header | `fjell/viewport_api.hpp` |
| Source | `src/scripting/viewport_api.cpp` |

## Syntax

```cpp
namespace fjell {

enum FjellViewportFlags : uint32_t {
    VP_TONEMAP        = 1 << 0,
    VP_BLOOM          = 1 << 1,
    VP_GTAO           = 1 << 2,
    VP_SSR            = 1 << 3,
    VP_VOLUMETRIC_FOG = 1 << 4,
    VP_HIZ            = 1 << 5,
    VP_SHADOWS        = 1 << 6,
    VP_MINIMAL        = VP_TONEMAP,
    VP_FULL           = VP_TONEMAP | VP_BLOOM | VP_GTAO | VP_SSR
                      | VP_VOLUMETRIC_FOG | VP_HIZ | VP_SHADOWS,
};

struct ViewportAPI {
    static uint32_t create            (const char* name, uint32_t width = 512, uint32_t height = 512,
                                       uint32_t flags = VP_MINIMAL);
    static uint32_t create_with_graph (const char* name, uint32_t width, uint32_t height,
                                       const char* graph_path);
    static void     destroy           (const char* name);
    static void     set_camera        (const char* name, uint32_t camera_node_id);
    static uint32_t bindless_index    (const char* name);
    static void     resize            (const char* name, uint32_t width, uint32_t height);
};

}
```

## Remarks

Each viewport owns an offscreen render target and a `PostProcessResources` slot. It runs an abbreviated frame for its camera each engine frame, with the set of post-process passes determined either by the flags bitmask or by a `.fjgraph` file.

Viewports are keyed by **name** (string). The name is also the suffix in material references: `"display": "$viewport:security_cam"`.

The returned `uint32_t` from `create` / `create_with_graph` is the **bindless texture index** — usable directly in shaders if you bypass the `$viewport:` material resolution. Most code doesn't need this; the material system handles it.

Cost scales with what you enable. `VP_SHADOWS` re-renders the shadow map for this viewport's camera — expensive, usually skip and let the viewport share the main view's shadows.

## Constants

### Feature flags

| Flag | Effect |
|---|---|
| `VP_TONEMAP` | Final AgX tone map. Without this the output is HDR-linear. |
| `VP_BLOOM` | Bloom pass. |
| `VP_GTAO` | Ground-truth AO. |
| `VP_SSR` | Screen-space reflections. |
| `VP_VOLUMETRIC_FOG` | Volumetric fog + godrays. |
| `VP_HIZ` | Hi-Z pyramid build (only useful if downstream passes need it). |
| `VP_SHADOWS` | Re-render shadow map for this viewport's camera. Expensive. |
| `VP_MINIMAL` | `VP_TONEMAP` only. |
| `VP_FULL` | Everything. |

## Functions

| Name | Description |
|---|---|
| [`create`](#viewportapicreate) | Create a viewport using flag bitmask |
| [`create_with_graph`](#viewportapicreate_with_graph) | Create a viewport driven by a `.fjgraph` file |
| [`destroy`](#viewportapidestroy) | Tear down a viewport |
| [`set_camera`](#viewportapiset_camera) | Point the viewport at a camera node |
| [`bindless_index`](#viewportapibindless_index) | Look up the bindless texture index |
| [`resize`](#viewportapiresize) | Resize the offscreen target |

---

## ViewportAPI::create

### Syntax
```cpp
static uint32_t create(const char* name,
                       uint32_t width = 512,
                       uint32_t height = 512,
                       uint32_t flags = VP_MINIMAL);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Identifier for this viewport. Used by `set_camera`, `destroy`, `resize`, and in material `$viewport:` refs. Must not be null. |
| `width` | `uint32_t` | Render target width in pixels. Default `512`. |
| `height` | `uint32_t` | Render target height in pixels. Default `512`. |
| `flags` | `uint32_t` | Bitwise OR of `VP_*` flags. Default `VP_MINIMAL`. |

### Return value
The bindless texture index. `0` if the name is already taken.

### Remarks
The viewport is created without a camera — call `set_camera` before the next frame for it to render anything meaningful. Until a camera is bound the texture is undefined (typically black).

---

## ViewportAPI::create_with_graph

### Syntax
```cpp
static uint32_t create_with_graph(const char* name,
                                  uint32_t width, uint32_t height,
                                  const char* graph_path);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Identifier for the viewport. |
| `width` / `height` | `uint32_t` | Render target dimensions. |
| `graph_path` | `const char*` | Path to a `.fjgraph` file, resolved relative to the engine's base path (typically under `assets/render_graphs/`). |

### Return value
Bindless texture index; `0` on failure (graph not found, name taken).

### Remarks
Cleaner than flags for complex setups. The `.fjgraph` defines which passes run, in what order, and which per-viewport images are allocated. Same hot-reload behavior as any other `.fjgraph`.

---

## ViewportAPI::destroy

### Syntax
```cpp
static void destroy(const char* name);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Name passed to `create`. Unknown names are silently ignored. |

### Remarks
Releases the offscreen target and frees the bindless slot. Any material still referencing `$viewport:name` will fall back to a black texture.

Always call this in `end_play()` if the script created the viewport — viewports do not auto-destroy with the script that created them.

---

## ViewportAPI::set_camera

### Syntax
```cpp
static void set_camera(const char* name, uint32_t camera_node_id);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Viewport identifier. |
| `camera_node_id` | `uint32_t` | Node ID carrying a `CameraComponent`. |

### Remarks
Safe to call every frame to track a moving camera node. Passing `0` or an ID without a camera component is a no-op (the previously bound camera stays).

---

## ViewportAPI::bindless_index

### Syntax
```cpp
[[nodiscard]] static uint32_t bindless_index(const char* name);
```

### Return value
The bindless texture index for the named viewport, or `0` if no viewport with that name exists.

### Remarks
Same value `create` returned, looked up by name. Almost never needed from script — the material system handles `$viewport:` resolution.

---

## ViewportAPI::resize

### Syntax
```cpp
static void resize(const char* name, uint32_t width, uint32_t height);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Viewport identifier. |
| `width` / `height` | `uint32_t` | New dimensions. |

### Remarks
Reallocates the offscreen target. Cheap-ish but not free — don't call every frame for animated zoom; if you need that, allocate the larger size up front and adjust the camera projection instead.

## Example

```cpp
class SecurityCamera : public fjell::ScriptComponent {
    FJELL_COMPONENT(SecurityCamera)

    void begin_play() override {
        fjell::ViewportAPI::create("security_cam", 1920, 1080, VP_FULL);
    }

    void tick(float /*dt*/) override {
        uint32_t cam = fjell::SceneAPI::find_node("SecurityCamera");
        if (cam != 0) {
            fjell::ViewportAPI::set_camera("security_cam", cam);
        }
    }

    void end_play() override {
        fjell::ViewportAPI::destroy("security_cam");
    }
};
```

Material side:

```json
{
    "shader": "shaders/monitor.fjsl",
    "textures": { "display": "$viewport:security_cam" }
}
```

Shader side:

```glsl
shader_type spatial;
render_mode unlit;
uniform sampler2D display;
void fragment() {
    ALBEDO = texture(display, UV).rgb;
    EMISSION = ALBEDO * 0.5;
}
```

## See also
- [Viewports](../../Viewports.md) — the full feature page
- [`CameraAPI`](CameraAPI.md) — main-swapchain camera control
- [FJMAT](../../Materials/FJMAT.md) — `$viewport:` reference syntax
