#pragma once

#include "gpu/access.hpp"
#include "gpu/format.hpp"
#include "renderer/pass_builder.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

/// A buffer a render subsystem's compute step writes and its draws then
/// read, with how the draws read it. A run that only draws what an earlier
/// run wrote declares the reads and not the write.
struct SubsystemDrawInput {
    BufferImport buffer;
    gpu::Access draw_access{gpu::Access::storage_buffer_read_vertex};
    /// Whether this run's compute step writes it.
    bool written{true};
};

/// State visible while the renderer's passes declare: the frame graph's
/// import catalogue, and what the render pipeline decided about this view
/// before any pass's declare() runs. Smaller than FrameContext on purpose:
/// only what is legal to read while the graph is being built (no command
/// buffer, no transient pointers).
struct DeclareContext : ImportCatalog {
    glm::uvec2 viewport_extent{0, 0};
    gpu::Samples msaa_samples{gpu::Samples::x1};
    uint32_t frame_index{0};
    /// The scene the viewport renders; 0 for none.
    uint64_t scene_id{0};

    /// Whether the device draws through mesh shaders, which decides which
    /// geometry path the depth prepass declares.
    bool mesh_shader_supported{false};

    /// Whether this run builds the bloom: bloom is on and this viewport's
    /// graph runs its chain. Off unless the render pipeline says so, so a
    /// reader never declares a bloom nothing writes.
    bool bloom_enabled{false};

    /// Whether the sea draws into this viewport this frame, and so whether
    /// "surface_depth" is an image of its own or another name for "depth".
    /// A pass that writes it declares that only when this is set.
    bool water_draws{false};
    /// Whether this run builds the volumetric fog's froxel volume, so a pass
    /// may declare a read of its integrated result.
    bool volumetric_fog_enabled{false};

    /// Whether this viewport shows the editor grid, and whether there are
    /// debug lines to draw. Each overlay draws over the post pass's output
    /// and hands it back to ImGui afterwards, so it declares that only in a
    /// frame it draws in.
    bool grid_shown{false};
    bool debug_lines{false};

    /// Bitmask of ViewportSource enum values: which debug visualizations
    /// are currently showing somewhere (viewport tabs, overlays). Viz
    /// passes check their bit in declare() and flag side-effects only
    /// when they're actually going to produce a visible output.
    uint32_t active_sources{0};

    /// What the render subsystems' compute step (particle simulation)
    /// writes for their own draws. The subsystems are pluggable, so the
    /// passes that call their draw hooks cannot name these buffers; they
    /// declare reads on the whole list, and the subsystem compute pass
    /// declares the writes.
    std::vector<SubsystemDrawInput> subsystem_draw_inputs;
};

} // namespace fjell
