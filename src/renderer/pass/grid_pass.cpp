#include "renderer/pass/grid_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
namespace fjell {

Result<> GridPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    if (auto made = overlay_.create(device_); !made) return made;

    std::array set_layouts = {resources.global_set_layout, overlay_.depth_set_layout()};
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(GridPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create grid pipeline layout");
    }

    // No vertex input: a fullscreen triangle from gl_VertexIndex, the plane
    // intersected per pixel. Blended over the output by the coverage the
    // fragment writes; the output's alpha stays opaque.
    VkShaderModule vert_module = create_shader_module(device_, read_spirv(base_path::resolve("shaders/grid.vert.spv")));
    VkShaderModule frag_module = create_shader_module(device_, read_spirv(base_path::resolve("shaders/grid.frag.spv")));
    pipeline_ = vk_utils::create_fullscreen_pipeline({
        .device = device_,
        .layout = layout_,
        .vert_module = vert_module,
        .frag_module = frag_module,
        .color_format = PostPass::OUTPUT_FORMAT,
        .blend_enable = true,
        .pipeline_cache = resources.pipeline_cache,
    });
    vkDestroyShaderModule(device_, frag_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);
    if (pipeline_ == VK_NULL_HANDLE) return make_error("Failed to create grid pipeline");

    FJELL_GFX_DEBUG("Grid pass pipeline created");
    return {};
}

void GridPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    overlay_.destroy();
}

PassSetup GridPass::setup() const {
    return {
        .name = "grid",
        .phase = RenderPhase::PostProcess,
        .sub_order = 310, // over the post pass's output (300), so no post
                          // shader grades or outlines it
    };
}

void GridPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.grid_shown) return;
    OutputOverlay::declare(builder, ctx);
}

void GridPass::record(const FrameContext& ctx) {
    if (!ctx.show_grid) return;

    FJELL_PROFILE_SCOPE_N("record_grid");
    FJELL_GPU_ZONE(ctx, "Grid");

    if (!overlay_.begin(ctx, layout_)) return;

    GridPushConstants push{};
    push.cell_size = settings_.cell_size;
    push.thickness = settings_.thickness;
    push.fade_distance = settings_.fade_distance;
    push.opacity = settings_.opacity;

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(GridPushConstants), &push);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);

    overlay_.end(ctx);
}

} // namespace fjell
