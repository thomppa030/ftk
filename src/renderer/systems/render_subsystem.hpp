#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

struct DeclareContext;
class Device;
namespace gpu {
class RenderEncoder;
}
class Project;
class ResourceManager;
class Scene;
struct FrameContext;
struct PassResources;

// Per-frame context passed to render subsystem update().
// Each subsystem reads what it needs and ignores the rest.
namespace gpu {
class Frame;
}

struct RenderSubsystemContext {
    Scene* scene{nullptr};
    ResourceManager* resource_manager{nullptr};
    const Project* project{nullptr};
    glm::mat4 view_projection{1.0f};
    glm::vec3 camera_position{0.0f};
    uint32_t frame_index{0};
    /// The frame recording, whose memory update() writes a view's data into,
    /// so each view's run keeps its own. Unset for sync().
    gpu::Frame* frame{nullptr};
    float dt{0.0f};
    bool playing{false};
};

// Base class for rendering subsystems (particles, sprites, tilemaps, etc.)
// that VulkanContext orchestrates through a common lifecycle.
//
// Lifecycle per frame:
//   1. sync()            — early, before command recording (rebuild GPU buffers)
//   2. update()          — gather scene data, upload SSBOs
//   3. record_compute()  — dispatch compute work (barriers, simulation)
//   4. record_opaque()   — draw during opaque color pass
//   5. record_transparent() — draw during transparent color pass
//
// The draw hooks are called inside the pass's render scope and are given its
// encoder. All record_* methods default to no-op. Override what you need.
class RenderSubsystem {
public:
    virtual ~RenderSubsystem() = default;

    virtual void create(Device& device, const PassResources& resources) = 0;
    virtual void destroy() = 0;

    // Early sync — called before command buffer recording.
    // Use for structural changes (add/remove emitters, rebuild buffers).
    virtual void sync(const RenderSubsystemContext& ctx) { (void)ctx; }

    // Frame update — gather data, sort, upload SSBOs.
    // Called after scene gathering, before render pipeline execution.
    virtual void update(const RenderSubsystemContext& ctx) { (void)ctx; }

    // Compute dispatch — called as a render pipeline pass (before color passes).
    virtual void record_compute(const FrameContext& ctx) { (void)ctx; }

    // Add what record_compute() writes for the draw hooks to
    // ctx.subsystem_draw_inputs, and anything another pass reads by name to
    // ctx.buffer_imports. Called once per pipeline run, before any pass
    // declares; a subsystem without compute work adds nothing.
    virtual void collect_exports(DeclareContext& ctx) const { (void)ctx; }

    // Draw into the depth pre-pass — called before the opaque pass so the
    // subsystem's opaque geometry contributes to Hi-Z, contact shadows,
    // and depth-driven post (SS-GI, SSR neighbour lookups).
    virtual void record_depth(const FrameContext& ctx, gpu::RenderEncoder& pass) { (void)ctx; (void)pass; }

    // Draw opaque geometry — called during the opaque color pass.
    virtual void record_opaque(const FrameContext& ctx, gpu::RenderEncoder& pass) { (void)ctx; (void)pass; }

    // Draw distortion particles into the distortion UV-offset buffer.
    virtual void record_distortion(const FrameContext& ctx, gpu::RenderEncoder& pass) { (void)ctx; (void)pass; }

    // Draw transparent geometry — called during the transparent color pass.
    // Subsystems are sorted by transparent_order() before recording (lower = drawn first = behind).
    virtual void record_transparent(const FrameContext& ctx, gpu::RenderEncoder& pass) { (void)ctx; (void)pass; }

    // Sort key for transparent pass draw order. Lower values draw first (behind).
    [[nodiscard]] virtual int transparent_order() const { return 0; }
};

} // namespace fjell
