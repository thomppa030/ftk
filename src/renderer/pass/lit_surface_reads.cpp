#include "renderer/pass/lit_surface_reads.hpp"

#include "renderer/pass_builder.hpp"

namespace fjell {

void declare_lit_surface_reads(PassBuilder& builder, const DeclareContext& ctx) {
    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, gpu::Access::sampled_fragment);
    }
    if (auto atlas = builder.import_named(ctx, "local_shadow_atlas"); atlas.valid()) {
        builder.read(atlas, gpu::Access::sampled_fragment);
    }
    if (auto cubemap = builder.import_named(ctx, "sky_cubemap"); cubemap.valid()) {
        builder.read(cubemap, gpu::Access::sampled_fragment);
    }
    builder.read(builder.import_named_buffer(ctx, "tile_light_ssbo"),
                 gpu::Access::storage_buffer_read_fragment);
    // A preview without GTAO has no AO image of its own.
    if (auto ao = builder.import_named_optional(ctx, "gtao_ao"); ao.valid()) {
        builder.read(ao, gpu::Access::sampled_fragment);
    }
    // Absent when the project's graph leaves the pass out.
    if (auto shadow = builder.import_named_optional(ctx, "contact_shadow"); shadow.valid()) {
        builder.read(shadow, gpu::Access::sampled_fragment);
    }
    if (auto shadow = builder.import_named_optional(ctx, "cloud_shadow"); shadow.valid()) {
        builder.read(shadow, gpu::Access::sampled_fragment);
    }
    if (auto motion = builder.import_named_optional(ctx, "motion_field"); motion.valid()) {
        builder.read(motion, gpu::Access::sampled_fragment);
    }
    // Absent while the global set binds the fallback in their place.
    if (auto gi = builder.import_named_optional(ctx, "gi_indirect"); gi.valid()) {
        builder.read(gi, gpu::Access::sampled_fragment);
    }
    if (auto gi = builder.import_named_optional(ctx, "ss_gi_emissive"); gi.valid()) {
        builder.read(gi, gpu::Access::sampled_fragment);
    }
}

} // namespace fjell
