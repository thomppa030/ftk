#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

#include <cstdint>

namespace fjell {

class Buffer;

class LightCullPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void collect_exports(DeclareContext& ctx) override;

private:
    gpu::Owned<gpu::ComputePipeline> pipeline_;

    /// This frame slot's tile lists, which the GPU context owns.
    const Buffer* output_{nullptr};
};

} // namespace fjell
