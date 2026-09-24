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
    bool two_pass_occlusion{false};

    // LOD stats
    uint32_t lod0_draws{0};
    uint32_t lod1_draws{0};
    uint32_t lod2_draws{0};
    uint32_t distance_culled{0};

    // Ray-traced GI: culled draws kept for the acceleration structure, and
    // instances in the structures built this frame.
    uint32_t gi_only_draws{0};
    uint32_t tlas_instances{0};

    // Terrain patches dispatched to the task shader for the camera view, and
    // the triangles they would emit if every one survived. The task shader
    // culls per patch on the GPU, so what actually rasterizes is at most this
    // and there is no readback to say by how much.
    uint32_t terrain_patches_dispatched{0};
    uint32_t terrain_triangles_max{0};

    void reset() {
        draw_calls = 0;
        triangle_count = 0;
        objects_total = 0;
        objects_culled = 0;
        indirect_draws = 0;
        meshlets_dispatched = 0;
        gpu_cull_active = false;
        two_pass_occlusion = false;
        lod0_draws = 0;
        lod1_draws = 0;
        lod2_draws = 0;
        distance_culled = 0;
        gi_only_draws = 0;
        tlas_instances = 0;
        terrain_patches_dispatched = 0;
        terrain_triangles_max = 0;
    }
};

} // namespace fjell
