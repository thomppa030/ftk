#pragma once

namespace fjell {

class GpuContext;
class PassRegistry;
struct PassResources;

/// Create all render passes, register them in the PassRegistry, and run
/// post_create() hooks (global descriptor writes, one-shot GPU work).
/// The PassResources must have draw_data_layout set by the caller.
void create_all_passes(PassRegistry& passes, GpuContext& gpu,
                       const PassResources& resources);

/// Create mesh shader passes (meshlet instance, cull, viz).
/// Called lazily on first mesh shader enable if not created at startup.
/// Returns false if any pass failed to create.
[[nodiscard]] bool create_meshlet_passes(PassRegistry& passes, GpuContext& gpu,
                                         const PassResources& resources);

} // namespace fjell
