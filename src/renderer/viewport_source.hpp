#pragma once

#include "ftk/gpu/binding.hpp"
#include "ftk/math/color_space.hpp"

#include <glm/glm.hpp>

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
    gi_indirect,
    gi_irradiance,
    motion_field,
};

// How many sources there are. Hand-kept, because a `count` sentinel in the
// enum would have to be handled by every switch over it. The assert below
// is what catches it falling behind: bump both together, and the view menu
// (ui/viewport_view.cpp) then asks for the new source's entry.
constexpr int VIEWPORT_SOURCE_COUNT = 13;
static_assert(static_cast<int>(ViewportSource::motion_field) == VIEWPORT_SOURCE_COUNT - 1,
              "VIEWPORT_SOURCE_COUNT must match the last ViewportSource");

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

    /// `.fjgraph` that decides which passes run for this viewport. Null
    /// falls back to RenderPipeline's global pass_state_ flags — used by
    /// the main viewport during the migration window before per-viewport
    /// graphs are wired everywhere. SceneViewports own their own graph.
    const FjGraph* viewport_graph{nullptr};

    /// Drawn in place of the sky when set; null shows the scene's sky.
    const SkyBackdrop* backdrop{nullptr};

    /// The editor's ground grid over this viewport. Each viewport has its
    /// own: a preview's toggle leaves the Scene's alone.
    bool show_grid{false};
};

} // namespace fjell
