#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

class Device;
class Scene;
struct FrameContext;
struct PassResources;

// Per-frame context passed to render subsystem update().
// Each subsystem reads what it needs and ignores the rest.
struct RenderSubsystemContext {
    Scene* scene{nullptr};
    glm::mat4 view_projection{1.0f};
    glm::vec3 camera_position{0.0f};
    uint32_t frame_index{0};
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
// All record_* methods default to no-op. Override what you need.
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

    // Draw opaque geometry — called during the opaque color pass.
    virtual void record_opaque(const FrameContext& ctx) { (void)ctx; }

    // Draw transparent geometry — called during the transparent color pass.
    virtual void record_transparent(const FrameContext& ctx) { (void)ctx; }
};

} // namespace fjell
