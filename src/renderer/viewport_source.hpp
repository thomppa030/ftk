#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

class OffscreenPass;

enum class ViewportSource : uint8_t {
    final_color,
    depth,
    wireframe,
    bloom,
};

inline const char* viewport_source_name(ViewportSource source) {
    switch (source) {
        case ViewportSource::final_color: return "Final Color";
        case ViewportSource::depth:       return "Depth";
        case ViewportSource::wireframe:   return "Wireframe";
        case ViewportSource::bloom:       return "Bloom";
    }
    return "Unknown";
}

constexpr int VIEWPORT_SOURCE_COUNT = 4;

struct ViewportRenderRequest {
    OffscreenPass* target{nullptr};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
};

} // namespace fjell
