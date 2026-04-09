#pragma once

#include <cstdint>

namespace fjell {

/// Image IDs registered with the FrameGraph at the start of each frame.
/// Passed to pass build callbacks so they can declare dependencies.
struct FrameImages {
    uint32_t color_id{0};
    uint32_t msaa_color_id{0};
    uint32_t depth_id{0};
    uint32_t msaa_depth_id{0};
    uint32_t mr_id{0};
    uint32_t msaa_mr_id{0};
    uint32_t normal_id{0};
    uint32_t msaa_normal_id{0};
    uint32_t screen_color_id{0};
    bool has_msaa{false};
};

} // namespace fjell
