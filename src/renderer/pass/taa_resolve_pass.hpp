#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace fjell {

class TaaResolvePass : public RenderPass {
public:
    // Per-viewport ping-pong history textures.
    struct Images {
        std::array<gpu::Owned<gpu::Texture>, 2> history;
        glm::uvec2 extent{0, 0};
        uint32_t write_index{0}; // ping-pong: write to [write_index], read from [1-write_index]
        bool needs_init{true};

        void create(gpu::Device& gpu, uint32_t width, uint32_t height);
        /// Releases the histories; the frames still reading them keep them
        /// until they are done.
        void destroy();
        void resize(gpu::Device& gpu, uint32_t width, uint32_t height);
    };

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    void record_into(const FrameContext& ctx, Images& imgs);

private:
    gpu::Owned<gpu::ComputePipeline> pipeline_;
    gpu::Sampler sampler_;
};

} // namespace fjell
