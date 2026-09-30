#pragma once

#include "renderer/pass/render_pass.hpp"

namespace fjell {

class AccelStructureManager;

/// Builds the ray tracing acceleration structure of the scene a viewport
/// renders, before any pass traces against it. Once per scene per frame:
/// the first viewport to get here builds it, and the rest trace what it
/// built. What goes in was decided before the frame was recorded
/// (AccelStructureManager::prepare_scene); this pass only records the build.
///
/// The TLAS and its instance table are exported under
/// AccelStructureManager::TLAS_NAME and INSTANCES_NAME and declared as this
/// pass's writes; the tracers declare their reads of them, so the graph
/// orders every trace after the build and keeps this pass while one reads.
class RtScenePass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    /// Null on a device without ray tracing, and then the pass does nothing.
    void set_accel_manager(AccelStructureManager* manager) { accel_manager_ = manager; }

private:
    AccelStructureManager* accel_manager_{nullptr};
};

} // namespace fjell
