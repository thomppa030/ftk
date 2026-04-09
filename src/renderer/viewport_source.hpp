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
    gi_indirect,
    gi_irradiance,
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
        case ViewportSource::gi_indirect:    return "GI Indirect";
        case ViewportSource::gi_irradiance:  return "GI Irradiance";
    }
    return "Unknown";
}

constexpr int VIEWPORT_SOURCE_COUNT = 14;

struct ViewportRenderRequest {
    OffscreenPass* target{nullptr};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    PostProcessResources* pp_resources{nullptr}; // null = use default
    bool skip_shadows{false};
    bool flip_tonemap_y{false}; // flip tonemap output for render-to-texture
};

} // namespace fjell
