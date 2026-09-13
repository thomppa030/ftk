#include "renderer/pass/opaque_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/pass/debug_pass.hpp"
#include "scene/scene.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <glm/glm.hpp>

namespace fjell {

Result<> OpaquePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void OpaquePass::destroy() {}

PassSetup OpaquePass::setup() const {
    return {
        .name = "color_opaque",
        .phase = RenderPhase::Color,
        .sub_order = 0,
    };
}

void OpaquePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // G-buffer writes. In non-MSAA mode the msaa_* names alias the
    // non-MSAA images in the catalog so both handles point at the
    // same VkImage — FrameGraph dedups them.
    auto msaa_color = builder.import_named(ctx, "msaa_color");
    auto color = builder.import_named(ctx, "color");
    auto msaa_mr = builder.import_named(ctx, "msaa_mr");
    auto mr = builder.import_named(ctx, "mr");
    auto msaa_normal = builder.import_named(ctx, "msaa_normal");
    auto normal = builder.import_named(ctx, "normal");
    auto msaa_velocity = builder.import_named(ctx, "msaa_velocity");
    auto velocity = builder.import_named(ctx, "velocity");
    auto msaa_depth = builder.import_named(ctx, "msaa_depth");
    auto depth = builder.import_named(ctx, "depth");

    builder.write(msaa_color, ResourceAccess::color_attachment);
    builder.write(color, ResourceAccess::color_attachment);
    builder.write(msaa_mr, ResourceAccess::color_attachment);
    builder.write(mr, ResourceAccess::color_attachment);
    builder.write(msaa_normal, ResourceAccess::color_attachment);
    builder.write(normal, ResourceAccess::color_attachment);
    builder.write(msaa_velocity, ResourceAccess::color_attachment);
    builder.write(velocity, ResourceAccess::color_attachment);
    builder.write(msaa_depth, ResourceAccess::depth_attachment);
    builder.write(depth, ResourceAccess::depth_attachment);

    // Consumed producer outputs. Declared reads here are what lets the
    // DAG see the cross-pass edges currently papered over by inline
    // finalize-to-SHADER_READ_ONLY barriers in the producers.
    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_fragment);
    }
    if (auto atlas = builder.import_named(ctx, "local_shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_fragment);
    }
    if (auto cubemap = builder.import_named(ctx, "sky_cubemap"); cubemap.valid()) {
        builder.read(cubemap, ResourceAccess::sampled_fragment);
    }
}

void OpaquePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_opaque");
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Color Opaque");
#endif
    auto* target = ctx.target;

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = target->msaa_color_view();
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {{0.1f, 0.1f, 0.12f, 1.0f}};

    bool msaa = target->sample_count() != VK_SAMPLE_COUNT_1_BIT;
    if (msaa) {
        color_attachment.resolveImageView = target->color_view();
        color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    VkRenderingAttachmentInfo mr_attachment{};
    mr_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    mr_attachment.imageView = target->msaa_mr_view();
    mr_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    mr_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    mr_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    mr_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    if (msaa) {
        mr_attachment.resolveImageView = target->mr_view();
        mr_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        mr_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    VkRenderingAttachmentInfo normal_attachment{};
    normal_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    normal_attachment.imageView = target->msaa_normal_view();
    normal_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    normal_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    normal_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    normal_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    if (msaa) {
        normal_attachment.resolveImageView = target->normal_view();
        normal_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        normal_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    VkRenderingAttachmentInfo velocity_attachment{};
    velocity_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    velocity_attachment.imageView = target->msaa_velocity_view();
    velocity_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    velocity_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    velocity_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    velocity_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    if (msaa) {
        velocity_attachment.resolveImageView = target->velocity_view();
        velocity_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        velocity_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    }

    std::array<VkRenderingAttachmentInfo, 4> color_attachments = {
        color_attachment, mr_attachment, normal_attachment, velocity_attachment};

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = target->msaa_depth_view();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (msaa) {
        depth_attachment.resolveImageView = target->depth_view();
        depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = target->extent();
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    ri.pColorAttachments = color_attachments.data();
    ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.cmd, &ri);

    VkViewport vp{};
    vp.width = static_cast<float>(target->extent().width);
    vp.height = static_cast<float>(target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    std::array descriptor_sets = {
        ctx.global_descriptor_set,
        ctx.bindless_set,
        ctx.draw_data_set};

    if (ctx.scene) {
#ifdef FJELL_ENABLE_TRACY
        { TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Opaques");
#endif
        std::array mesh_descriptor_sets = {
            ctx.global_descriptor_set,
            ctx.bindless_set,
            ctx.draw_data_set,
            ctx.meshlet_set};

        const auto& ranges = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
        auto indirect_buf = ctx.gpu_driven_indirect;

        if (indirect_buf && !ranges.empty()) {
            auto draw_indirect = ctx.draw_mesh_tasks_indirect_fn;
            uint32_t seg_size = ctx.gpu_driven_segment_size;

            for (uint32_t bin = 0; bin < static_cast<uint32_t>(ranges.size()); ++bin) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  ctx.pipeline_cache->gpu_driven_pipeline(ranges[bin].pipeline_id));
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        ctx.pipeline_cache->gpu_driven_layout(),
                                        0, static_cast<uint32_t>(mesh_descriptor_sets.size()),
                                        mesh_descriptor_sets.data(), 0, nullptr);

                uint32_t bin_offset = bin * seg_size;
                vkCmdPushConstants(ctx.cmd, ctx.pipeline_cache->gpu_driven_layout(),
                                  VK_SHADER_STAGE_TASK_BIT_EXT,
                                  0, sizeof(uint32_t), &bin_offset);

                VkDeviceSize cmd_offset = bin * sizeof(VkDrawMeshTasksIndirectCommandEXT);
                draw_indirect(ctx.cmd, indirect_buf, cmd_offset, 1,
                              sizeof(VkDrawMeshTasksIndirectCommandEXT));
            }
        }

        ctx.render_stats->meshlets_dispatched +=
            ctx.indirect_batch->total_meshlet_instances(ctx.frame_index);
        ctx.render_stats->gpu_cull_active = true;
        ctx.render_stats->draw_calls += ctx.indirect_batch->opaque_draw_count(ctx.frame_index);
        ctx.render_stats->triangle_count += ctx.indirect_batch->total_triangles(ctx.frame_index);

        // Draw skinned meshes (per-model VBO, not in indirect batch)
        {
            const auto& skinned_indices = ctx.indirect_batch->skinned_draw_indices(ctx.frame_index);
            uint32_t skinned_cmd_idx = 0;
            for (const auto& dc : ctx.queue->commands()) {
                if (!dc.skinned || !dc.skeletal_model) continue;
                if (skinned_cmd_idx >= skinned_indices.size()) break;

                auto pipeline = ctx.pipeline_cache->skinned_opaque_pipeline(dc.pipeline_id);
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        ctx.pipeline_cache->layout(),
                                        0, static_cast<uint32_t>(descriptor_sets.size()),
                                        descriptor_sets.data(), 0, nullptr);

                dc.skeletal_model->bind(ctx.cmd);
                uint32_t draw_data_idx = skinned_indices[skinned_cmd_idx];
                vkCmdDrawIndexed(ctx.cmd, dc.skeletal_model->index_count(), 1, 0, 0, draw_data_idx);
                ctx.render_stats->draw_calls++;
                skinned_cmd_idx++;
            }
        }
#ifdef FJELL_ENABLE_TRACY
        }
#endif
    }

    for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
        ctx.subsystems[i]->record_opaque(ctx);
    }

    // Before sky: terrain is opaque geometry and writes depth.
    ctx.passes->get<TerrainPass>("terrain").record(ctx);

    ctx.passes->get<SkyPass>("sky").record(ctx);
    ctx.passes->get<DebugPass>("debug").record(ctx);
    draw_outline(ctx);

    vkCmdEndRendering(ctx.cmd);
}

void OpaquePass::draw_outline(const FrameContext& ctx) {
    if (ctx.selected_id == 0 || !ctx.scene) return;

    const auto* obj = ctx.scene->find(ctx.selected_id);
    if (!obj) return;
    const auto* spatial = obj->as_spatial();
    if (!spatial) return;
    const auto* mesh = obj->components.get<MeshComponent>();
    if (!mesh || !mesh->model || !mesh->material) return;

    auto* cache = ctx.pipeline_cache;
    uint32_t pid = mesh->material->pipeline_id();
    auto pipeline = cache->outline_pipeline(pid);
    auto layout = cache->outline_layout();

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    std::array descriptor_sets = {ctx.global_descriptor_set,
                                   ctx.bindless_set,
                                   ctx.draw_data_set};
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, static_cast<uint32_t>(descriptor_sets.size()),
                            descriptor_sets.data(), 0, nullptr);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(ctx.cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

    // Push: outline_color vec4 + thickness float
    struct {
        glm::vec4 color;
        float thickness;
    } push{};
    push.color = {1.0f, 0.6f, 0.15f, 0.7f};

    // The shader offsets in NDC, which spans 2 units across the viewport, so a
    // fixed NDC thickness would grow with resolution. Convert from pixels.
    constexpr float outline_px = 2.0f;
    const float width = static_cast<float>(ctx.target->extent().width);
    push.thickness = width > 0.0f ? (outline_px * 2.0f) / width : 0.003f;

    vkCmdPushConstants(ctx.cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    // Find the draw index for this object in the draw data SSBO
    uint32_t draw_index = 0;
    bool found = false;
    for (const auto& dc : ctx.queue->commands()) {
        if (dc.node_id == ctx.selected_id) { found = true; break; }
        draw_index++;
    }
    if (!found) return;

    // Draw with firstInstance = draw_index so gl_BaseInstance reads the right DrawData
    ctx.mega_buffer->bind(ctx.cmd);
    mesh->model->draw(ctx.cmd, draw_index);
}

} // namespace fjell
