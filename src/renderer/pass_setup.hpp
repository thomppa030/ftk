#pragma once

#include "ftk/base/result.hpp"

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
/// An entry whose `enabled_if` names an unsupported device feature (mesh
/// shaders, currently the only one) is skipped rather than constructed —
/// see the `feature_available` switch in the .cpp.
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

} // namespace fjell
