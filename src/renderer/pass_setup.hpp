#pragma once

#include "core/result.hpp"

namespace fjell {

class GpuContext;
class PassRegistry;
class RenderPipeline;
struct FjGraph;
struct PassResources;

/// Create all render passes from a parsed `.fjgraph`, register them in the
/// PassRegistry and the RenderPipeline, run post_create() hooks, and
/// apply per-pass `enabled` flags from the graph. Each pass is
/// instantiated via `PassFactory::make` keyed on the entry's `class`.
///
/// Mesh shader passes are *not* in the graph — they're conditional on
/// `gpu.use_mesh_shaders()` and continue to be created via
/// `create_meshlet_passes()` from the call site. Future revisions can
/// move them into the graph with `enabled_if: "mesh_shaders"`.
[[nodiscard]] Result<> create_passes_from_graph(const FjGraph& graph,
                                                PassRegistry& passes,
                                                RenderPipeline& pipeline,
                                                GpuContext& gpu,
                                                const PassResources& resources);

/// Re-apply per-pass `enabled` flags from a re-parsed graph to the live
/// pipeline. This is the v1 hot-reload payload: pass shape changes
/// (adds/removes/params) are not honored — only the boolean toggle.
/// Callers diff old vs new graphs themselves to log a warning when shape
/// changed.
void apply_enabled_flags(const FjGraph& graph, RenderPipeline& pipeline);

/// Create mesh shader passes (meshlet instance, cull, viz).
/// Called lazily on first mesh shader enable if not created at startup.
/// Returns false if any pass failed to create.
[[nodiscard]] bool create_meshlet_passes(PassRegistry& passes, GpuContext& gpu,
                                         const PassResources& resources);

} // namespace fjell
