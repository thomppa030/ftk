// viewport_api.hpp — Static viewport API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdint>

/// Post-process passes a viewport runs. OR them together — each one costs
/// extra GPU time per frame, so enable only what the surface actually shows.
enum FjellViewportFlags : uint32_t {
    VP_TONEMAP        = 1 << 0,  ///< Map HDR to displayable range. Without it the image looks washed out.
    VP_BLOOM          = 1 << 1,  ///< Glow around bright areas.
    VP_GTAO           = 1 << 2,  ///< Ambient occlusion in creases and corners.
    VP_SSR            = 1 << 3,  ///< Screen-space reflections.
    VP_VOLUMETRIC_FOG = 1 << 4,  ///< Light shafts and depth fog.
    VP_HIZ            = 1 << 5,  ///< Occlusion culling. Helps in dense scenes, costs more in sparse ones.
    VP_SHADOWS        = 1 << 6,  ///< Re-render shadow maps from this camera. Expensive; usually skip.
    VP_MINIMAL        = VP_TONEMAP,  ///< Tonemap only — the sane default for small displays.
    VP_FULL           = VP_TONEMAP | VP_BLOOM | VP_GTAO | VP_SSR | VP_VOLUMETRIC_FOG | VP_HIZ | VP_SHADOWS,
                                     ///< Everything. For a mirror or portal that must match the main view.
};

extern "C" {
FJELL_SDK_EXPORT uint32_t fjell_viewport_create(const char* name, uint32_t width, uint32_t height,
                                uint32_t flags);
/// Newer entry point: pick a `.fjgraph` for the viewport instead of a
/// flag bitmask. Topology, per-viewport image alloc, and pass enabling
/// all flow from the graph file. `graph_path` is resolved relative to
/// the engine's base path (typically `assets/render_graphs/foo.fjgraph`).
FJELL_SDK_EXPORT uint32_t fjell_viewport_create_with_graph(const char* name,
                                uint32_t width, uint32_t height,
                                const char* graph_path);
FJELL_SDK_EXPORT void     fjell_viewport_destroy(const char* name);
FJELL_SDK_EXPORT void     fjell_viewport_set_camera(const char* name, uint32_t camera_node_id);
FJELL_SDK_EXPORT uint32_t fjell_viewport_bindless_index(const char* name);
FJELL_SDK_EXPORT void     fjell_viewport_resize(const char* name, uint32_t width, uint32_t height);
}

namespace fjell {

/// Renders the scene from an extra camera into a texture that materials can
/// sample — security monitors, mirrors, portals, minimaps.
///
/// A viewport is a full scene render per frame, so keep the resolution modest
/// and the flags lean. Create it once in `begin_play()`, point it at a camera,
/// and destroy it when done.
///
/// @code
/// void begin_play() override {
///     fjell::ViewportAPI::create("security_cam", 512, 512, VP_MINIMAL);
///     fjell::ViewportAPI::set_camera("security_cam",
///         fjell::SceneAPI::find_node("CamNode"));
/// }
/// void end_play() override { fjell::ViewportAPI::destroy("security_cam"); }
/// @endcode
/// A material samples it by naming it: `"display": "$viewport:security_cam"`.
struct ViewportAPI {
    /// Create a viewport that renders the scene to a texture.
    /// @param name   Your identifier for it — every other call takes this, and
    ///               `.fjmat` files reference it as `$viewport:name`.
    /// @param width  Render width in pixels. Powers of two are cheapest.
    /// @param height Render height in pixels.
    /// @param flags  OR'd `FjellViewportFlags`. `VP_MINIMAL` is the default;
    ///               `VP_FULL` costs the most.
    /// @return Bindless texture index, or 0 on failure. Materials using
    ///         `$viewport:` rebind automatically, so the value is rarely needed.
    static uint32_t create(const char* name, uint32_t width = 512, uint32_t height = 512,
                           uint32_t flags = VP_MINIMAL) {
        return fjell_viewport_create(name, width, height, flags);
    }

    /// Create a viewport whose pipeline is described by a `.fjgraph` file
    /// instead of a flag bitmask. Preferred for anything non-trivial: pass
    /// topology and image allocation live in the graph, versioned with the project.
    /// @param name       Your identifier, as in `create()`.
    /// @param width      Render width in pixels.
    /// @param height     Render height in pixels.
    /// @param graph_path Path to the `.fjgraph`, relative to the project base
    ///                   (e.g. `assets/render_graphs/mirror.fjgraph`).
    /// @return Bindless texture index, or 0 on failure.
    static uint32_t create_with_graph(const char* name, uint32_t width, uint32_t height,
                                      const char* graph_path) {
        return fjell_viewport_create_with_graph(name, width, height, graph_path);
    }

    /// Destroy a viewport and free its render targets. Materials referencing
    /// it fall back to a placeholder rather than breaking.
    /// @param name The identifier passed to `create()`.
    static void destroy(const char* name) {
        fjell_viewport_destroy(name);
    }

    /// Choose which camera the viewport renders from. Call it once unless the
    /// view should switch; the viewport follows that node's transform on its own.
    /// @param name           The identifier passed to `create()`.
    /// @param camera_node_id A node carrying a CameraNode.
    static void set_camera(const char* name, uint32_t camera_node_id) {
        fjell_viewport_set_camera(name, camera_node_id);
    }

    /// Bindless texture index for this viewport's output, for code that binds
    /// textures directly. Materials should use `$viewport:name` instead.
    /// @param name The identifier passed to `create()`.
    /// @return The index, or 0 if no viewport has that name.
    static uint32_t bindless_index(const char* name) {
        return fjell_viewport_bindless_index(name);
    }

    /// Change a viewport's render resolution, reallocating its targets. Not
    /// cheap — avoid calling it every frame.
    /// @param name   The identifier passed to `create()`.
    /// @param width  New width in pixels.
    /// @param height New height in pixels.
    static void resize(const char* name, uint32_t width, uint32_t height) {
        fjell_viewport_resize(name, width, height);
    }
};

} // namespace fjell
