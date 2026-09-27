// sprite_sheet.hpp — Sprite sheet definition for game scripts.
// Layout-matching copy of renderer/sprite_sheet.hpp, shipped as part of the SDK.
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

/// One animation's worth of frames packed into a grid on a single texture.
struct SpriteSheet {
    /// Path to the sheet texture, relative to the project root.
    std::string texture_path;

    /// Size of one frame in pixels. Every frame in a sheet is the same size.
    glm::ivec2 frame_size{0, 0};

    /// Number of frames to play. May be fewer than the grid holds, when the
    /// last row is partly empty.
    uint32_t frame_count{0};

    /// Frames per row in the grid, used to turn a frame index into a cell.
    uint32_t columns{0};

    /// Playback rate in frames per second.
    float fps{10.0f};

    /// Whether playback repeats. Non-looping sheets drive
    /// `sprite_anim::is_finished()`.
    bool loop{true};

    /// Per-frame UV rectangles, derived from the fields above when the sheet
    /// loads. Engine-owned — do not populate by hand.
    std::vector<glm::vec4> frame_uvs;
};

} // namespace fjell
