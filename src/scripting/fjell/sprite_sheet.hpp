// sprite_sheet.hpp — Sprite sheet definition for game scripts.
// Layout-matching copy of renderer/sprite_sheet.hpp, shipped as part of the SDK.
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct SpriteSheet {
    std::string texture_path;
    glm::ivec2 frame_size{0, 0};
    uint32_t frame_count{0};
    uint32_t columns{0};
    float fps{10.0f};
    bool loop{true};

    std::vector<glm::vec4> frame_uvs;
};

} // namespace fjell
