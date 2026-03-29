#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

class OffscreenPass;
class PostProcessResources;

enum class ViewportSource : uint8_t {
    final_color,
    depth,
    wireframe,
    bloom,
    normals,
    overdraw,
    uv,
    light_heatmap,
    light_grid,
    meshlet_viz,
    ao,
    gpu_cull,
};

inline const char* viewport_source_name(ViewportSource source) {
    switch (source) {
        case ViewportSource::final_color:    return "Final Color";
        case ViewportSource::depth:          return "Depth";
        case ViewportSource::wireframe:      return "Wireframe";
        case ViewportSource::bloom:          return "Bloom";
        case ViewportSource::normals:        return "Normals";
        case ViewportSource::overdraw:       return "Overdraw";
        case ViewportSource::uv:             return "UV";
        case ViewportSource::light_heatmap:  return "Light Heatmap";
        case ViewportSource::light_grid:     return "Light Grid";
        case ViewportSource::meshlet_viz:    return "Meshlet Viz";
        case ViewportSource::ao:             return "Ambient Occlusion";
        case ViewportSource::gpu_cull:       return "GPU Cull";
    }
    return "Unknown";
}

constexpr int VIEWPORT_SOURCE_COUNT = 12;

struct ViewportRenderRequest {
    OffscreenPass* target{nullptr};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    PostProcessResources* pp_resources{nullptr}; // null = use default
    bool skip_shadows{false};
};

} // namespace fjell
