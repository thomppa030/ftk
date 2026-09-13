#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

/// Camera state a viewport rendered with on its previous frame.
///
/// Motion vectors and every temporal pass (TAA, volumetric fog, SS-GI)
/// reproject the current frame through these matrices, so each viewport keeps
/// its own copy. A single shared copy would let the last viewport to render
/// decide what "previous frame" means for every other one, which hands the
/// main viewport a camera it never looked through and makes its temporal
/// passes reject their history every frame.
struct ViewHistory {
    glm::mat4 view{1.0f};
    glm::mat4 view_proj{1.0f};            // jittered projection * view
    glm::mat4 unjittered_view_proj{1.0f};
    uint32_t taa_frame_count{0};          // position in the jitter sequence

    /// Record the camera this viewport is rendering with now so the next frame
    /// can reproject against it.
    void advance(const glm::mat4& current_view, const glm::mat4& jittered_projection,
                 const glm::mat4& projection) {
        view = current_view;
        view_proj = jittered_projection * current_view;
        unjittered_view_proj = projection * current_view;
    }
};

} // namespace fjell
