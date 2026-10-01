#include "renderer/render_queue.hpp"
#include "ftk/base/profiler.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/render_stats.hpp"

#include <algorithm>

namespace fjell {

void RenderQueue::clear() {
    opaque_.clear();
    transparent_.clear();
}

void RenderQueue::reserve(size_t count) {
    opaque_.reserve(count);
    transparent_.reserve(count / 4); // transparent is typically a small fraction
}

void RenderQueue::push(const DrawCommand& cmd) {
    if (cmd.transparent) {
        transparent_.push_back(cmd);
    } else {
        opaque_.push_back(cmd);
    }
}

void RenderQueue::sort(const glm::vec3& /*camera_pos*/) {
    FJELL_PROFILE_SCOPE_N("render_queue_sort");
    // Opaque: sort by material to minimize descriptor rebinds
    std::sort(opaque_.begin(), opaque_.end(),
              [](const DrawCommand& a, const DrawCommand& b) {
                  return a.sort_key < b.sort_key;
              });

    // Transparent: sort back-to-front (distance precomputed during culling)
    std::sort(transparent_.begin(), transparent_.end(),
              [](const DrawCommand& a, const DrawCommand& b) {
                  return a.camera_distance > b.camera_distance;
              });
}

uint64_t RenderQueue::make_sort_key(uint32_t pipeline_id, uint32_t material_id, uint32_t mesh_id,
                                    bool skinned) {
    // [skinned:1][pipeline_id:15][material_id:24][mesh_id:24]
    return (static_cast<uint64_t>(skinned ? 1 : 0) << 63) |
           (static_cast<uint64_t>(pipeline_id & 0x7FFF) << 48) |
           (static_cast<uint64_t>(material_id & 0xFFFFFF) << 24) |
           (static_cast<uint64_t>(mesh_id & 0xFFFFFF));
}

} // namespace fjell
