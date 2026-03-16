#pragma once

#include <cstdint>

namespace fjell {

struct RenderStats {
    uint32_t draw_calls{0};
    uint32_t triangle_count{0};

    void reset() {
        draw_calls = 0;
        triangle_count = 0;
    }
};

} // namespace fjell
