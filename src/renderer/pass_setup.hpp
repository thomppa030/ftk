#pragma once

#include "core/result.hpp"

namespace fjell {

class GpuContext;
class PassRegistry;
struct FjGraph;
struct PassResources;

/// Create all render passes from a parsed `.fjgraph`, register them in the
/// PassRegistry, and run post_create() hooks. Each pass is instantiated
/// via `PassFactory::make` keyed on the entry's `class` field.
///
/// Mesh shader passes are *not* in the graph — they're conditional on
/// `gpu.use_mesh_shaders()` and continue to be created via
/// `create_meshlet_passes()` from the call site. Future revisions can
/// move them into the graph with `enabled_if: "mesh_shaders"`.
[[nodiscard]] Result<> create_passes_from_graph(const FjGraph& graph,
                                                PassRegistry& passes,
                                                GpuContext& gpu,
                                                const PassResources& resources);

/// Create mesh shader passes (meshlet instance, cull, viz).
/// Called lazily on first mesh shader enable if not created at startup.
/// Returns false if any pass failed to create.
[[nodiscard]] bool create_meshlet_passes(PassRegistry& passes, GpuContext& gpu,
                                         const PassResources& resources);

} // namespace fjell
