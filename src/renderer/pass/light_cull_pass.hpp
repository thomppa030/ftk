#pragma once

#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

#include <cstdint>

namespace fjell {

class LightCullPass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void collect_exports(DeclareContext& ctx) override;

private:
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> pipeline_;

    /// This frame slot's tile lists, which the GPU context owns.
    ftk::gpu::Buffer output_{};
};

} // namespace fjell
