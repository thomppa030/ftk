#pragma once

#include "core/math/color_space.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class OffscreenPass;
class PostProcessResources;
struct FjGraph;

enum class ViewportSource : uint8_t {
    final_color,
    depth,
    wireframe,
    normals,
    overdraw,
    uv,
    light_heatmap,
    light_grid,
    meshlet_viz,
    ao,
    gpu_cull,
    gi_indirect,
    gi_irradiance,
};

inline const char* viewport_source_name(ViewportSource source) {
    switch (source) {
        case ViewportSource::final_color:    return "Final Color";
        case ViewportSource::depth:          return "Depth";
        case ViewportSource::wireframe:      return "Wireframe";
        case ViewportSource::normals:        return "Normals";
        case ViewportSource::overdraw:       return "Overdraw";
        case ViewportSource::uv:             return "UV";
        case ViewportSource::light_heatmap:  return "Light Heatmap";
        case ViewportSource::light_grid:     return "Light Grid";
        case ViewportSource::meshlet_viz:    return "Meshlet Viz";
        case ViewportSource::ao:             return "Ambient Occlusion";
        case ViewportSource::gpu_cull:       return "GPU Cull";
        case ViewportSource::gi_indirect:    return "GI Indirect";
        case ViewportSource::gi_irradiance:  return "GI Irradiance";
    }
    return "Unknown";
}

constexpr int VIEWPORT_SOURCE_COUNT = 13;

/// A flat gradient drawn where the scene's sky would be, for a preview
/// whose subject should not compete with an HDRI. Only the picture changes:
/// the environment lighting still comes from the scene's sky. Linear
/// colours, like everything the renderer multiplies.
struct SkyBackdrop {
    glm::vec3 top{0.0f};
    glm::vec3 bottom{0.0f};

    /// The editor's studio backdrop for asset previews: dark neutral grey,
    /// a little lighter towards the top.
    static const SkyBackdrop& editor_preview() {
        static const SkyBackdrop backdrop{
            .top = color_space::srgb_to_linear(glm::vec3{0.227f, 0.239f, 0.259f}),
            .bottom = color_space::srgb_to_linear(glm::vec3{0.102f, 0.110f, 0.122f}),
        };
        return backdrop;
    }
};

struct ViewportRenderRequest {
    OffscreenPass* target{nullptr};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    PostProcessResources* pp_resources{nullptr}; // null = use default
    bool skip_shadows{false};
    bool flip_tonemap_y{false}; // flip tonemap output for render-to-texture

    /// Private global descriptor set for this offscreen viewport. When set,
    /// render_preview copies the per-frame global bindings into it and
    /// overwrites the view-dependent images here, instead of mutating the
    /// shared per-frame set the main command buffer binds (which races a
    /// pending CB → DEVICE_LOST). Null = legacy shared-set path.
    VkDescriptorSet global_set{VK_NULL_HANDLE};

    /// `.fjgraph` that decides which passes run for this viewport. Null
    /// falls back to RenderPipeline's global pass_state_ flags — used by
    /// the main viewport during the migration window before per-viewport
    /// graphs are wired everywhere. SceneViewports own their own graph.
    const FjGraph* viewport_graph{nullptr};

    /// Drawn in place of the sky when set; null shows the scene's sky.
    const SkyBackdrop* backdrop{nullptr};
};

} // namespace fjell
