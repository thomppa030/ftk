#pragma once

#include "core/named_value.hpp"
#include "environment/environment_state.hpp"
#include "renderer/resources/fjsl_parser.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fjell {

/// What a layer is drawn with: the shader, the material row its textures come
/// from, and what those texture slots hold. Two volumes with the same shader
/// and the same textures are one look with different values, and blend their
/// values; anything else blends its finished image.
struct PostLook {
    uint32_t pipeline_id{0};
    uint32_t material_id{0};
    /// The shader's own texture slots as the row holds them, the PBR set left
    /// out and slots the shader does not declare left zero, so two rows that
    /// differ only where nothing samples still count as the same look.
    std::array<uint32_t, CUSTOM_TEXTURE_SLOTS> textures{};

    /// Whether values can blend between the two. The row itself may differ:
    /// two .fjmat of one shader with the same textures are the same look.
    [[nodiscard]] bool same_look(const PostLook& other) const {
        return pipeline_id == other.pipeline_id && textures == other.textures;
    }
};

/// One fullscreen draw of the post pass: a look, its values, and how much of
/// it lies over the layers below.
struct PostLayer {
    PostLook look;
    std::vector<NamedValue> values;
    float weight{1.0f};
};

/// A post process volume the pass can draw: the volume as the scene resolved
/// it, and the look the pass found for it (its material's, or the engine
/// default's when it has none or its shader cannot draw).
struct PostCandidate {
    const PostVolume* volume{nullptr};
    PostLook look;
};

/// The layers one viewport draws, lowest first, for a camera at `camera`.
///
/// Every volume the camera is in contributes its weight times how far inside
/// it stands; an infinite one contributes its weight wherever the camera is.
/// They apply lowest priority first over `engine_default`, which starts at
/// weight 1. A volume of the same look as the layer below blends its values
/// into that layer by its contribution; one of another look starts a layer of
/// its own at that weight. A layer at full weight covers everything under it,
/// so what was under it is dropped and it becomes the first.
[[nodiscard]] std::vector<PostLayer> plan_post_layers(const PostLayer& engine_default,
                                                      std::span<const PostCandidate> volumes,
                                                      const glm::vec3& camera);

} // namespace fjell
