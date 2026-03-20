#pragma once

#include <cstdint>

namespace fjell {

struct RenderStats {
    uint32_t draw_calls{0};
    uint32_t triangle_count{0};
    uint32_t objects_total{0};
    uint32_t objects_culled{0};
    uint32_t indirect_draws{0};
    uint32_t meshlets_dispatched{0};
    bool gpu_cull_active{false};

    void reset() {
        draw_calls = 0;
        triangle_count = 0;
        objects_total = 0;
        objects_culled = 0;
        indirect_draws = 0;
        meshlets_dispatched = 0;
        gpu_cull_active = false;
    }
};

} // namespace fjell
