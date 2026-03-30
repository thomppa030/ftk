#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct SpriteSheet {
    std::string texture_path;       // relative asset path to the sprite sheet PNG
    glm::ivec2 frame_size{0, 0};    // pixel size of each frame
    uint32_t frame_count{0};        // total number of frames
    uint32_t columns{0};            // frames per row (0 = auto from texture width)
    float fps{10.0f};
    bool loop{true};

    // Computed at load time
    std::vector<glm::vec4> frame_uvs; // normalized {u_min, v_min, u_max, v_max} per frame
};

/// Load a .fjsheet JSON file. frame_uvs are NOT computed yet (needs texture dimensions).
[[nodiscard]] SpriteSheet load_sprite_sheet(const std::string& path);

/// Save a .fjsheet JSON file.
void save_sprite_sheet(const std::string& path, const SpriteSheet& sheet);

/// Compute frame_uvs from texture dimensions. Call after loading the texture.
void compute_frame_uvs(SpriteSheet& sheet, uint32_t tex_width, uint32_t tex_height);

} // namespace fjell
