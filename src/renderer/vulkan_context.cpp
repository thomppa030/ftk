#include "renderer/vulkan_context.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "animation/animation_system.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "core/window.hpp"
#include "scene/scene.hpp"
#include "scene/light_node.hpp"

#include <imgui.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fjell {

VulkanContext::VulkanContext(Window& window)
    : gpu_{window}
    , target_pool_{gpu_.device(), gpu_.allocator()} {
    FJELL_GFX_INFO("Initializing Vulkan context");

    // Scene gathering: indirect batches + draw data descriptors
    gatherer_.init(gpu_);

    // Init pipelines now that draw_data_layout is available
    gpu_.init_pipelines(gatherer_.draw_data_descriptor().layout());

    // Build PassResources with draw_data_layout
    PassResources pass_resources = gpu_.pass_resources();
    pass_resources.draw_data_layout = gatherer_.draw_data_descriptor().layout();

    // Helper: pass creation is init-time — propagate Result failures as throws
    auto check_pass = [](Result<>&& r) {
        if (!r) throw std::runtime_error(r.error());
    };

    check_pass(depth_prepass_.create(gpu_.device_ref(), pass_resources));
    check_pass(hiz_pass_.create(gpu_.device_ref(), pass_resources));
    if (gpu_.use_mesh_shaders()) {
        meshlet_instance_pass_.create(gpu_.device(), gpu_.allocator(),
                                       gpu_.global_set_layout(), pass_resources.pipeline_cache);
        check_pass(mesh_shader_depth_pass_.create(gpu_.device_ref(), pass_resources));
        check_pass(meshlet_viz_pass_.create(gpu_.device_ref(), pass_resources));
        check_pass(cull_pass_.create(gpu_.device(), gpu_.allocator(),
                                     gpu_.global_set_layout(), pass_resources.pipeline_cache));
        check_pass(cull_viz_pass_.create(gpu_.device_ref(), pass_resources));
        meshlet_passes_created_ = true;
    }
    check_pass(shadow_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(depth_viz_pass_.create(gpu_.device_ref(), pass_resources));

    update_shadow_descriptors();

    check_pass(debug_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(grid_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(wireframe_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(normal_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(overdraw_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(uv_viz_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(bloom_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(sky_pass_.create(gpu_.device_ref(), pass_resources));

    // Sky cubemap for IBL (creates cubemap + BRDF LUT)
    sky_cubemap_pass_.create(gpu_.device(), gpu_.allocator(),
                              gpu_.command_pool(), gpu_.graphics_queue(),
                              pass_resources.pipeline_cache);
    sky_cubemap_pass_.set_colors(sky_pass_.top_color(), sky_pass_.bottom_color());

    // Generate cubemap (one-shot at startup)
    {
        VkCommandBuffer init_cmd;
        VkCommandBufferAllocateInfo cmd_ai{};
        cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_ai.commandPool = gpu_.command_pool();
        cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(gpu_.device(), &cmd_ai, &init_cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(init_cmd, &begin);

        sky_cubemap_pass_.generate(init_cmd);

        vkEndCommandBuffer(init_cmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &init_cmd;
        vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(gpu_.graphics_queue());
        vkFreeCommandBuffers(gpu_.device(), gpu_.command_pool(), 1, &init_cmd);
    }

    // Write sky cubemap (binding 6) and BRDF LUT (binding 7) into global descriptors
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo cubemap_info{};
        cubemap_info.sampler = sky_cubemap_pass_.cubemap_sampler();
        cubemap_info.imageView = sky_cubemap_pass_.cubemap_view();
        cubemap_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo brdf_info{};
        brdf_info.sampler = sky_cubemap_pass_.brdf_lut_sampler();
        brdf_info.imageView = sky_cubemap_pass_.brdf_lut_view();
        brdf_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = gpu_.global_descriptor_set(i);
        writes[0].dstBinding = 6;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &cubemap_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = gpu_.global_descriptor_set(i);
        writes[1].dstBinding = 7;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &brdf_info;

        vkUpdateDescriptorSets(gpu_.device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    check_pass(transparent_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(tonemap_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(gtao_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(ssr_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(volumetric_fog_pass_.create(gpu_.device_ref(), pass_resources));
    check_pass(light_cull_pass_.create(gpu_.device_ref(), pass_resources));
    light_heatmap_pass_.set_mode(0);
    check_pass(light_heatmap_pass_.create(gpu_.device_ref(), pass_resources));
    light_grid_pass_.set_mode(1);
    check_pass(light_grid_pass_.create(gpu_.device_ref(), pass_resources));
    scene_sdf_pass_.create(gpu_.device_ref(), pass_resources.global_set_layout, pass_resources.allocator,
                            pass_resources.pipeline_cache);

    // Write scene SDF (binding 9) into global descriptors — must be after pass creation
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo sdf_info{};
        sdf_info.sampler = scene_sdf_pass_.sampler();
        sdf_info.imageView = scene_sdf_pass_.view();
        sdf_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet sdf_write{};
        sdf_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sdf_write.dstSet = gpu_.global_descriptor_set(i);
        sdf_write.dstBinding = 9;
        sdf_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sdf_write.descriptorCount = 1;
        sdf_write.pImageInfo = &sdf_info;
        vkUpdateDescriptorSets(gpu_.device(), 1, &sdf_write, 0, nullptr);
    }

    particle_system_.create(gpu_.device_ref(), pass_resources);
    FJELL_GFX_DEBUG("Creating sprite renderer");
    sprite_renderer_.create(gpu_.device_ref(), pass_resources);
    tilemap_renderer_.create(gpu_.device_ref(), pass_resources);
    FJELL_GFX_DEBUG("Outline pipelines managed by PipelineCache");
    FJELL_GFX_DEBUG("Creating debug draw");
    debug_draw_ = std::make_unique<DebugDraw>(gpu_.device(), gpu_.allocator());

    register_passes();

    // Default post-process resources for the main viewport (all passes enabled)
    PostProcessConfig default_config{};
    default_config.ssr = true;
    default_config.volumetric_fog = true;
    PostProcessResources::PassRefs refs{hiz_pass_, gtao_pass_, bloom_pass_, ssr_pass_, volumetric_fog_pass_, tonemap_pass_};
    default_pp_ = std::make_unique<PostProcessResources>(gpu_.device(), gpu_.allocator(), 256, 256, default_config, refs);

    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    auto vk_device = gpu_.device();

    default_pp_.reset();
    debug_draw_.reset();
    tilemap_renderer_.destroy();
    sprite_renderer_.destroy();
    scene_sdf_pass_.destroy();
    particle_system_.destroy();
    light_grid_pass_.destroy();
    light_heatmap_pass_.destroy();
    gtao_pass_.destroy();
    ssr_pass_.destroy();
    volumetric_fog_pass_.destroy();

    // Previous frame color for SSR
    if (prev_frame_color_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(vk_device, prev_frame_color_view_, nullptr);
    }
    destroy_vma_image(gpu_.allocator(), prev_frame_color_, prev_frame_color_allocation_);

    light_cull_pass_.destroy();
    sky_cubemap_pass_.destroy();
    sky_pass_.destroy();
    transparent_pass_.destroy();
    tonemap_pass_.destroy();
    bloom_pass_.destroy();
    uv_viz_pass_.destroy();
    overdraw_pass_.destroy();
    normal_pass_.destroy();
    wireframe_pass_.destroy();
    grid_pass_.destroy();
    debug_pass_.destroy();
    depth_viz_pass_.destroy();
    shadow_pass_.destroy();
    depth_prepass_.destroy();
    mesh_shader_depth_pass_.destroy();
    meshlet_viz_pass_.destroy();
    cull_viz_pass_.destroy();
    cull_pass_.destroy();
    meshlet_instance_pass_.destroy();
    hiz_pass_.destroy();

    // SceneGatherer (gatherer_) destroyed automatically — owns its
    // indirect batches and draw data descriptors.
    // GpuContext (gpu_) destroyed automatically — owns device, swapchain,
    // command pools, descriptors, sync objects, mega_buffer, pipelines, etc.
}

void VulkanContext::set_use_mesh_shaders(bool enable) {
    gpu_.set_use_mesh_shaders(enable);

    // Lazily create meshlet passes on first enable
    if (enable && gpu_.use_mesh_shaders() && !meshlet_passes_created_) {
        PassResources pass_resources = gpu_.pass_resources();
        pass_resources.draw_data_layout = gatherer_.draw_data_descriptor().layout();

        meshlet_instance_pass_.create(gpu_.device(), gpu_.allocator(),
                                       gpu_.global_set_layout(), pass_resources.pipeline_cache);
        auto r1 = mesh_shader_depth_pass_.create(gpu_.device_ref(), pass_resources);
        auto r2 = meshlet_viz_pass_.create(gpu_.device_ref(), pass_resources);
        auto r3 = cull_pass_.create(gpu_.device(), gpu_.allocator(),
                                     gpu_.global_set_layout(), pass_resources.pipeline_cache);
        auto r4 = cull_viz_pass_.create(gpu_.device_ref(), pass_resources);
        if (!r1 || !r2 || !r3 || !r4) {
            FJELL_GFX_ERROR("Failed to create meshlet passes — disabling mesh shaders");
            gpu_.set_use_mesh_shaders(false);
            return;
        }
        meshlet_passes_created_ = true;

        // Register custom vertex displacement depth pipelines for the newly
        // created mesh shader depth pass (vertex prepass + shadow already have theirs).
        auto* pc = gpu_.pipeline_cache();
        if (pc) {
            for (uint32_t id = 0; id < pc->shader_count(); ++id) {
                if (!pc->has_custom_vertex(id)) continue;
                const auto& depth_mesh = pc->depth_mesh_spv(id);
                if (!depth_mesh.empty()) {
                    mesh_shader_depth_pass_.register_custom_depth(id, depth_mesh);
                }
            }
        }

        FJELL_GFX_INFO("Meshlet passes created (lazy init)");
    }
}

void VulkanContext::sync_sprite_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    sprite_renderer_.sync_textures(scene, resources, project);
}

void VulkanContext::sync_tilemap_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    tilemap_renderer_.sync_textures(scene, resources, project);
}

// ── Pass registration ────────────────────────────────────────────────────

void VulkanContext::register_passes() {
    register_environment_passes();
    register_depth_passes();
    register_gpu_driven_passes();
    register_color_passes();
    register_post_process_passes();
    register_debug_passes();
    register_pass_resources();
}

void VulkanContext::register_environment_passes() {
    render_pipeline_.register_pass({
        .name = "sky_cubemap",
        .order = 50,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            graph.add_pass("sky_cubemap", [this](VkCommandBuffer cb) {
                sky_cubemap_pass_.set_colors(sky_pass_.top_color(), sky_pass_.bottom_color());
                if (sky_cubemap_pass_.dirty()) {
                    sky_cubemap_pass_.generate(cb);
                }
            }, {});
        }
    });

    // Shadow pass — uses its own batch with per-cascade culled objects
    render_pipeline_.register_pass({
        .name = "shadow",
        .order = 100,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            graph.add_pass("shadow", [this](VkCommandBuffer cb) {
                if (current_ctx_.skip_shadows) return;
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Shadow Pass");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
                    local.shadow_cascade_batches[c] = gatherer_.shadow_batch(c);
                    local.shadow_cascade_draw_data[c] =
                        gatherer_.shadow_draw_data(c).descriptor_set(current_frame_);
                    local.shadow_cascade_commands[c] = &gatherer_.shadow_cmds(c);
                }
                shadow_pass_.record(local);
            }, {}, 1);
        }
    });
}

void VulkanContext::register_depth_passes() {
    render_pipeline_.register_pass({
        .name = "depth_prepass",
        .order = 200,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            auto& depth_pass = gpu_.use_mesh_shaders()
                ? static_cast<RenderPass&>(mesh_shader_depth_pass_)
                : static_cast<RenderPass&>(depth_prepass_);

            if (imgs.has_msaa) {
                graph.add_pass("depth_prepass", [this, &depth_pass](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                    TracyVkZone(gpu_.tracy_ctx(), cb, "Depth Prepass");
#endif
                    FrameContext local = current_ctx_;
                    local.cmd = cb;
                    local.gpu_driven_indirect = VK_NULL_HANDLE;
                    local.gpu_driven_segment_size = 0;
                    depth_pass.record(local);
                }, {{imgs.msaa_depth_id, ImageUsage::depth_attachment}}, 1);

                graph.add_pass("depth_resolve", [this](VkCommandBuffer cb) {
                    auto* target = current_ctx_.target;
                    VkRenderingAttachmentInfo depth_attachment{};
                    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    depth_attachment.imageView = target->msaa_depth_view();
                    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    depth_attachment.resolveImageView = target->depth_view();
                    depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    depth_attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

                    VkRenderingInfo ri{};
                    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                    ri.renderArea.extent = target->extent();
                    ri.layerCount = 1;
                    ri.pDepthAttachment = &depth_attachment;

                    vkCmdBeginRendering(cb, &ri);
                    vkCmdEndRendering(cb);
                }, {{imgs.msaa_depth_id, ImageUsage::depth_attachment_read},
                    {imgs.depth_id, ImageUsage::depth_attachment}});
            } else {
                graph.add_pass("depth_prepass", [this, &depth_pass](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                    TracyVkZone(gpu_.tracy_ctx(), cb, "Depth Prepass");
#endif
                    FrameContext local = current_ctx_;
                    local.cmd = cb;
                    local.gpu_driven_indirect = VK_NULL_HANDLE;
                    local.gpu_driven_segment_size = 0;
                    depth_pass.record(local);
                }, {{imgs.depth_id, ImageUsage::depth_attachment}}, 1);
            }
        }
    });

    // Hi-Z pyramid build
    render_pipeline_.register_pass({
        .name = "hiz_build",
        .order = 300,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("hiz_build", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Hi-Z Build");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                if (local.pp_resources && local.pp_resources->hiz()) {
                    hiz_pass_.record_into(local, *local.pp_resources->hiz());
                }
            }, {{imgs.depth_id, ImageUsage::compute_read}});
        }
    });
}

void VulkanContext::register_gpu_driven_passes() {
    render_pipeline_.register_pass({
        .name = "meshlet_cull",
        .order = 400,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            graph.add_pass("meshlet_expand_cull", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Meshlet Expand+Cull");
#endif
                if (gpu_.use_mesh_shaders()) {
                    uint32_t total_draws = gatherer_.indirect_batch()->opaque_draw_count(current_frame_);
                    uint32_t total_mi = gatherer_.indirect_batch()->total_meshlet_instances(current_frame_);
                    if (total_draws > 0 && total_mi > 0) {
                        meshlet_instance_pass_.dispatch_expand(
                            cb,
                            gatherer_.indirect_batch()->draw_data_buffer(current_frame_),
                            gatherer_.indirect_batch()->draw_data_size(current_frame_),
                            total_draws, current_frame_);
                        meshlet_instance_pass_.insert_expand_barrier(cb);

                        auto& ranges_ref = gatherer_.indirect_batch()->pipeline_ranges(current_frame_);
                        auto extent = current_ctx_.target->extent();

                        auto* hiz_imgs = current_ctx_.pp_resources ? current_ctx_.pp_resources->hiz() : nullptr;
                        bool hiz_ready = hiz_imgs && hiz_imgs->ready();

                        meshlet_instance_pass_.dispatch_cull_first(
                            cb,
                            gpu_.global_descriptor_set(current_frame_),
                            gatherer_.indirect_batch()->draw_data_buffer(current_frame_),
                            gatherer_.indirect_batch()->draw_data_size(current_frame_),
                            gpu_.meshlet_buffer() ? gpu_.meshlet_buffer()->bounds_buffer() : VK_NULL_HANDLE,
                            gpu_.meshlet_buffer() ? gpu_.meshlet_buffer()->bounds_buffer_size() : 0,
                            gatherer_.indirect_batch()->pipeline_bin_buffer(current_frame_),
                            gatherer_.indirect_batch()->pipeline_bin_size(current_frame_),
                            static_cast<uint32_t>(ranges_ref.size()),
                            current_frame_,
                            hiz_ready ? hiz_imgs->hiz_sampled_view : VK_NULL_HANDLE,
                            hiz_ready ? hiz_pass_.sampler() : VK_NULL_HANDLE,
                            extent.width, extent.height,
                            hiz_ready ? hiz_imgs->hiz_mip_count : 0);

                        meshlet_instance_pass_.insert_cull_barrier(cb);
                        gpu_.render_stats().two_pass_occlusion = hiz_ready;
                    }
                }
            }, {});
        }
    });

    // GTAO
    render_pipeline_.register_pass({
        .name = "gtao",
        .order = 500,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("gtao", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "GTAO");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                if (local.pp_resources && local.pp_resources->gtao()) {
                    gtao_pass_.record_into(local, *local.pp_resources->gtao());
                }
            }, {{imgs.depth_id, ImageUsage::compute_read}});
        }
    });

    // Light culling
    render_pipeline_.register_pass({
        .name = "light_cull",
        .order = 510,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("light_cull", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Light Cull");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                light_cull_pass_.record(local);

                VkMemoryBarrier2 mem_barrier{};
                mem_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mem_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mem_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mem_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                mem_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &mem_barrier;
                vkCmdPipelineBarrier2(cb, &dep);
            }, {{imgs.depth_id, ImageUsage::compute_read}});
        }
    });

    // Particle compute
    render_pipeline_.register_pass({
        .name = "particle_compute",
        .order = 520,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            graph.add_pass("particle_compute", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Particle Compute");
#endif
                if (current_ctx_.scene) {
                    auto& scene_ref = const_cast<Scene&>(*current_ctx_.scene);
                    auto vp = gatherer_.projection_matrix() * gatherer_.view_matrix();
                    particle_system_.dispatch_compute(cb, current_ctx_.delta_time, scene_ref,
                                                      current_ctx_.global_descriptor_set,
                                                      gatherer_.camera_position(), vp);
                    particle_system_.insert_barriers(cb);
                }
            }, {});
        }
    });

    // Cull visualization
    render_pipeline_.register_pass({
        .name = "cull_viz",
        .order = 530,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::gpu_cull)))) return;
            graph.add_pass("cull_viz", [this](VkCommandBuffer cb) {
                if (gpu_.use_mesh_shaders() && cull_pass_.enabled()) {
                    FrameContext local = current_ctx_;
                    local.cmd = cb;
                    uint32_t opaque_count = gatherer_.indirect_batch()->opaque_draw_count(current_frame_);
                    if (opaque_count > 0) {
                        cull_viz_pass_.record(local,
                            cull_pass_.visibility_buffer(current_frame_),
                            cull_pass_.visibility_buffer_size(current_frame_));
                    }
                }
            }, {});
        }
    });
}

void VulkanContext::register_color_passes() {
    render_pipeline_.register_pass({
        .name = "color_opaque",
        .order = 600,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("color_opaque", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Color Opaque");
#endif
                auto* target = current_ctx_.target;

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

                std::array<VkRenderingAttachmentInfo, 2> color_attachments = {color_attachment, mr_attachment};

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

                vkCmdBeginRendering(cb, &ri);

                FrameContext local = current_ctx_;
                local.cmd = cb;

                VkViewport vp{};
                vp.width = static_cast<float>(target->extent().width);
                vp.height = static_cast<float>(target->extent().height);
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cb, 0, 1, &vp);

                VkRect2D sc{};
                sc.extent = target->extent();
                vkCmdSetScissor(cb, 0, 1, &sc);

                std::array descriptor_sets = {
                    gpu_.global_descriptor_set(current_frame_),
                    gpu_.bindless_textures().descriptor_set(),
                    gatherer_.draw_data_descriptor().descriptor_set(current_frame_)};

                if (current_ctx_.scene) {
#ifdef FJELL_ENABLE_TRACY
                    { TracyVkZone(gpu_.tracy_ctx(), cb, "Opaques");
#endif
                    if (gpu_.use_mesh_shaders() && gpu_.pipeline_cache()->has_mesh_shaders()) {
                        std::array mesh_descriptor_sets = {
                            gpu_.global_descriptor_set(current_frame_),
                            gpu_.bindless_textures().descriptor_set(),
                            gatherer_.draw_data_descriptor().descriptor_set(current_frame_),
                            gpu_.meshlet_descriptor().descriptor_set(current_frame_)};

                        const auto& ranges = gatherer_.indirect_batch()->pipeline_ranges(current_frame_);
                        auto indirect_buf = meshlet_instance_pass_.indirect_buffer(current_frame_);

                        if (indirect_buf && !ranges.empty()) {
                            auto draw_indirect = gpu_.device_ref().draw_mesh_tasks_indirect_fn();
                            uint32_t seg_size = meshlet_instance_pass_.segment_size(current_frame_);

                            for (uint32_t bin = 0; bin < static_cast<uint32_t>(ranges.size()); ++bin) {
                                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  gpu_.pipeline_cache()->gpu_driven_pipeline(ranges[bin].pipeline_id));
                                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                        gpu_.pipeline_cache()->gpu_driven_layout(),
                                                        0, static_cast<uint32_t>(mesh_descriptor_sets.size()),
                                                        mesh_descriptor_sets.data(), 0, nullptr);

                                uint32_t bin_offset = bin * seg_size;
                                vkCmdPushConstants(cb, gpu_.pipeline_cache()->gpu_driven_layout(),
                                                  VK_SHADER_STAGE_TASK_BIT_EXT,
                                                  0, sizeof(uint32_t), &bin_offset);

                                VkDeviceSize cmd_offset = bin * sizeof(VkDrawMeshTasksIndirectCommandEXT);
                                draw_indirect(cb, indirect_buf, cmd_offset, 1,
                                              sizeof(VkDrawMeshTasksIndirectCommandEXT));
                            }
                        }

                        gpu_.render_stats().meshlets_dispatched +=
                            gatherer_.indirect_batch()->total_meshlet_instances(current_frame_);
                        gpu_.render_stats().gpu_cull_active = true;
                    } else {
                        gpu_.mega_buffer()->bind(cb);

                        const auto& ranges = gatherer_.indirect_batch()->pipeline_ranges(current_frame_);
                        for (const auto& range : ranges) {
                            auto pipeline = range.skinned
                                ? gpu_.pipeline_cache()->skinned_opaque_pipeline(range.pipeline_id)
                                : gpu_.pipeline_cache()->opaque_pipeline(range.pipeline_id);
                            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    gpu_.pipeline_cache()->layout(),
                                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                                    descriptor_sets.data(), 0, nullptr);

                            VkDeviceSize offset = range.first_draw * sizeof(VkDrawIndexedIndirectCommand);
                            vkCmdDrawIndexedIndirect(cb,
                                                     gatherer_.indirect_batch()->indirect_buffer(current_frame_),
                                                     offset, range.draw_count,
                                                     sizeof(VkDrawIndexedIndirectCommand));
                            gpu_.render_stats().indirect_draws++;
                        }
                    }
                    gpu_.render_stats().draw_calls += gatherer_.indirect_batch()->opaque_draw_count(current_frame_);
                    gpu_.render_stats().triangle_count += gatherer_.indirect_batch()->total_triangles(current_frame_);

                    // Draw skinned meshes (per-model VBO, not in indirect batch)
                    {
                        const auto& skinned_indices = gatherer_.indirect_batch()->skinned_draw_indices(current_frame_);
                        uint32_t skinned_cmd_idx = 0;
                        for (const auto& dc : gatherer_.render_queue().commands()) {
                            if (!dc.skinned || !dc.skeletal_model) continue;
                            if (skinned_cmd_idx >= skinned_indices.size()) break;

                            auto pipeline = gpu_.pipeline_cache()->skinned_opaque_pipeline(dc.pipeline_id);
                            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    gpu_.pipeline_cache()->layout(),
                                                    0, static_cast<uint32_t>(descriptor_sets.size()),
                                                    descriptor_sets.data(), 0, nullptr);

                            dc.skeletal_model->bind(cb);
                            uint32_t draw_data_idx = skinned_indices[skinned_cmd_idx];
                            vkCmdDrawIndexed(cb, dc.skeletal_model->index_count(), 1, 0, 0, draw_data_idx);
                            gpu_.render_stats().draw_calls++;
                            skinned_cmd_idx++;
                        }
                    }
#ifdef FJELL_ENABLE_TRACY
                    }
#endif
                }

                // Opaque particle rendering (mesh debris, solid particles)
                particle_system_.record_render_opaque(local);

                sky_pass_.record(local);
                grid_pass_.record(local);
                debug_pass_.record(local);
                draw_outline(cb, local);
                tilemap_renderer_.record_render(local);
                sprite_renderer_.record_render(local);

                vkCmdEndRendering(cb);
            }, {{imgs.msaa_color_id, ImageUsage::color_attachment},
                {imgs.color_id, ImageUsage::color_attachment},
                {imgs.msaa_mr_id, ImageUsage::color_attachment},
                {imgs.mr_id, ImageUsage::color_attachment},
                {imgs.msaa_depth_id, ImageUsage::depth_attachment},
                {imgs.depth_id, ImageUsage::depth_attachment}});
        }
    });

    // ── Screen color copy: snapshot resolved color for SCREEN_TEXTURE ───
    render_pipeline_.register_pass({
        .name = "screen_color_copy",
        .order = 625,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("screen_color_copy", [this](VkCommandBuffer cb) {
                auto* target = current_ctx_.target;
                VkExtent2D ext = target->extent();

                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {ext.width, ext.height, 1};

                vkCmdCopyImage(cb,
                    target->color_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    target->screen_color_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region);
            }, {{imgs.color_id, ImageUsage::transfer_src},
                {imgs.screen_color_id, ImageUsage::transfer_dst}});
        }
    });

    // ── Color pass: transparent geometry + worldspace UI + particles ─────
    render_pipeline_.register_pass({
        .name = "color_transparent",
        .order = 650,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("color_transparent", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Color Transparent");
#endif
                auto* target = current_ctx_.target;

                VkRenderingAttachmentInfo color_attachment{};
                color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color_attachment.imageView = target->msaa_color_view();
                color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                bool msaa = target->sample_count() != VK_SAMPLE_COUNT_1_BIT;
                if (msaa) {
                    color_attachment.resolveImageView = target->color_view();
                    color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                }

                // MR not needed for transparent objects (they don't write MR)
                VkRenderingAttachmentInfo mr_attachment{};
                mr_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                mr_attachment.imageView = target->msaa_mr_view();
                mr_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                mr_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                mr_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                if (msaa) {
                    mr_attachment.resolveImageView = target->mr_view();
                    mr_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    mr_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                }

                std::array<VkRenderingAttachmentInfo, 2> color_attachments = {color_attachment, mr_attachment};

                VkRenderingAttachmentInfo depth_attachment{};
                depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth_attachment.imageView = target->msaa_depth_view();
                depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

                if (msaa) {
                    depth_attachment.resolveImageView = VK_NULL_HANDLE;
                    depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
                }

                VkRenderingInfo ri{};
                ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.renderArea.extent = target->extent();
                ri.layerCount = 1;
                ri.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
                ri.pColorAttachments = color_attachments.data();
                ri.pDepthAttachment = &depth_attachment;

                vkCmdBeginRendering(cb, &ri);

                FrameContext local = current_ctx_;
                local.cmd = cb;

                VkViewport vp{};
                vp.width = static_cast<float>(target->extent().width);
                vp.height = static_cast<float>(target->extent().height);
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cb, 0, 1, &vp);

                VkRect2D sc{};
                sc.extent = target->extent();
                vkCmdSetScissor(cb, 0, 1, &sc);

                transparent_pass_.record(local);

                // Worldspace UI: renders after transparent objects, before particles
                if (world_ui_callback_ && playing_) {
                    world_ui_callback_(cb,
                        gpu_.global_descriptor_set(current_frame_),
                        gpu_.bindless_textures().descriptor_set(),
                        target->extent(), current_frame_);
                }

                particle_system_.record_render(local);

                vkCmdEndRendering(cb);
            }, {{imgs.msaa_color_id, ImageUsage::color_attachment},
                {imgs.color_id, ImageUsage::color_attachment},
                {imgs.msaa_mr_id, ImageUsage::color_attachment},
                {imgs.mr_id, ImageUsage::color_attachment},
                {imgs.msaa_depth_id, ImageUsage::shader_read},
                {imgs.depth_id, ImageUsage::shader_read},
                {imgs.screen_color_id, ImageUsage::shader_read}});
        }
    });
}

void VulkanContext::register_post_process_passes() {
    render_pipeline_.register_pass({
        .name = "ssr",
        .order = 700,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("ssr", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "SSR");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                // Hi-Z is lazily created during pipeline execution (order 300),
                // so we must read it here, not from the pre-built FrameContext.
                auto* hiz_imgs = local.pp_resources ? local.pp_resources->hiz() : nullptr;
                if (hiz_imgs && hiz_imgs->ready()) {
                    local.hiz_view = hiz_imgs->hiz_sampled_view;
                    local.hiz_sampler = hiz_pass_.sampler();
                    local.hiz_mip_count = hiz_imgs->hiz_mip_count;
                }
                if (local.pp_resources && local.pp_resources->ssr()) {
                    ssr_pass_.record_into(local, *local.pp_resources->ssr());
                }
            }, {{imgs.depth_id, ImageUsage::compute_read},
                {imgs.color_id, ImageUsage::compute_write},
                {imgs.mr_id, ImageUsage::compute_read}});
        }
    });

    // Volumetric fog
    render_pipeline_.register_pass({
        .name = "volumetric_fog",
        .order = 800,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("volumetric_fog", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Volumetric Fog");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                if (local.pp_resources && local.pp_resources->volumetric_fog()) {
                    volumetric_fog_pass_.record_into(local, *local.pp_resources->volumetric_fog());
                }
            }, {{imgs.depth_id, ImageUsage::compute_read},
                {imgs.color_id, ImageUsage::compute_write}});
        }
    });

    // Bloom
    render_pipeline_.register_pass({
        .name = "bloom",
        .order = 900,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("bloom", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Bloom");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                if (local.pp_resources && local.pp_resources->bloom()) {
                    bloom_pass_.record_into(local, *local.pp_resources->bloom());
                }
            }, {{imgs.color_id, ImageUsage::shader_read}});
        }
    });

    // Tonemap
    render_pipeline_.register_pass({
        .name = "tonemap",
        .order = 1000,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            graph.add_pass("tonemap", [this](VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
                TracyVkZone(gpu_.tracy_ctx(), cb, "Tonemap");
#endif
                FrameContext local = current_ctx_;
                local.cmd = cb;
                if (local.pp_resources && local.pp_resources->tonemap()) {
                    tonemap_pass_.record_into(local, *local.pp_resources->tonemap(), current_ctx_.target->color_view());
                }
            }, {{imgs.color_id, ImageUsage::shader_read}});
        }
    });
}

void VulkanContext::register_debug_passes() {
    render_pipeline_.register_pass({
        .name = "depth_viz",
        .order = 1100,
        .build = [this](FrameGraph& graph, const FrameImages& imgs) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::depth)))) return;
            graph.add_pass("depth_viz", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                depth_viz_pass_.record(local);
            }, {{imgs.depth_id, ImageUsage::shader_read}});
        }
    });

    render_pipeline_.register_pass({
        .name = "wireframe",
        .order = 1110,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::wireframe)))) return;
            graph.add_pass("wireframe", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                wireframe_pass_.record(local);
            }, {});
        }
    });

    render_pipeline_.register_pass({
        .name = "normals",
        .order = 1120,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::normals)))) return;
            graph.add_pass("normals", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                normal_pass_.record(local);
            }, {});
        }
    });

    render_pipeline_.register_pass({
        .name = "overdraw",
        .order = 1130,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::overdraw)))) return;
            graph.add_pass("overdraw", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                overdraw_pass_.record(local);
            }, {});
        }
    });

    render_pipeline_.register_pass({
        .name = "uv_viz",
        .order = 1140,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::uv)))) return;
            graph.add_pass("uv_viz", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                uv_viz_pass_.record(local);
            }, {});
        }
    });

    render_pipeline_.register_pass({
        .name = "light_heatmap",
        .order = 1150,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::light_heatmap)))) return;
            graph.add_pass("light_heatmap", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                light_heatmap_pass_.set_tile_ssbo(current_ctx_.tile_light_ssbo,
                                                   current_ctx_.tile_light_ssbo_size);
                light_heatmap_pass_.record(local);
            }, {});
        }
    });

    render_pipeline_.register_pass({
        .name = "light_grid",
        .order = 1160,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::light_grid)))) return;
            graph.add_pass("light_grid", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                light_grid_pass_.set_tile_ssbo(current_ctx_.tile_light_ssbo,
                                                current_ctx_.tile_light_ssbo_size);
                light_grid_pass_.record(local);
            }, {});
        }
    });

    render_pipeline_.register_pass({
        .name = "meshlet_viz",
        .order = 1170,
        .build = [this](FrameGraph& graph, const FrameImages& /*imgs*/) {
            if (!(active_sources_ & (1u << static_cast<uint32_t>(ViewportSource::meshlet_viz)))) return;
            graph.add_pass("meshlet_viz", [this](VkCommandBuffer cb) {
                FrameContext local = current_ctx_;
                local.cmd = cb;
                meshlet_viz_pass_.record(local);
            }, {});
        }
    });
}

void VulkanContext::register_pass_resources() {
    render_pipeline_.set_pass_resources("depth_prepass", {}, {"depth"});
    render_pipeline_.set_pass_resources("hiz_build", {"depth"}, {"hiz"});
    render_pipeline_.set_pass_resources("gtao", {"depth"}, {"ao"});
    render_pipeline_.set_pass_resources("color_opaque", {"depth", "ao"}, {"scene_color", "metallic_roughness"});
    render_pipeline_.set_pass_resources("screen_color_copy", {"scene_color"}, {"screen_color"});
    render_pipeline_.set_pass_resources("color_transparent", {"screen_color"}, {"scene_color"});
    render_pipeline_.set_pass_resources("ssr", {"depth", "scene_color", "metallic_roughness", "hiz"}, {"scene_color"});
    render_pipeline_.set_pass_resources("volumetric_fog", {"depth", "scene_color"}, {"scene_color"});
    render_pipeline_.set_pass_resources("bloom", {"scene_color"}, {"scene_color"});
    render_pipeline_.set_pass_resources("tonemap", {"scene_color"}, {"tonemapped"});
}

// ── Command buffer recording ────────────────────────────────────────────

void VulkanContext::render_scene_to(VkCommandBuffer cmd, OffscreenPass& target,
                                    Scene* scene, float dt, uint32_t selected_id,
                                    ThreadPool& pool, PostProcessResources* pp,
                                    bool skip_shadows, bool flip_tonemap_y) {
    FJELL_PROFILE_SCOPE_N("render_scene_to");

    // Lazily create / resize previous-frame color buffer for SSR.
    // Skip for viewports that don't use SSR (e.g. camera preview) to avoid
    // vkDeviceWaitIdle mid-command-buffer when the extent differs.
    bool needs_ssr_buffer = pp && pp->ssr();
    if (needs_ssr_buffer) {
        auto ext = target.extent();
        if (prev_frame_color_ == VK_NULL_HANDLE ||
            ext.width != prev_frame_extent_.width ||
            ext.height != prev_frame_extent_.height) {
            auto vk = gpu_.device();
            vkDeviceWaitIdle(vk);

            if (prev_frame_color_view_ != VK_NULL_HANDLE) {
                vkDestroyImageView(vk, prev_frame_color_view_, nullptr);
                prev_frame_color_view_ = VK_NULL_HANDLE;
            }
            destroy_vma_image(gpu_.allocator(), prev_frame_color_, prev_frame_color_allocation_);
            prev_frame_color_ = VK_NULL_HANDLE;
            prev_frame_color_allocation_ = VK_NULL_HANDLE;

            prev_frame_extent_ = ext;

            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            ci.extent = {ext.width, ext.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            create_vma_image(gpu_.allocator(), &ci, &prev_frame_color_, &prev_frame_color_allocation_);

            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = prev_frame_color_;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(vk, &vi, nullptr, &prev_frame_color_view_);

            ssr_pass_.resize(ext.width, ext.height);

            // Clear prev_frame to black and transition to SHADER_READ_ONLY
            // so first-frame SSR reads valid (black) data.
            VkCommandBuffer init_cmd;
            VkCommandBufferAllocateInfo cmd_ai{};
            cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cmd_ai.commandPool = gpu_.command_pool();
            cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmd_ai.commandBufferCount = 1;
            vkAllocateCommandBuffers(vk, &cmd_ai, &init_cmd);

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(init_cmd, &begin);

            transition_image(init_cmd, prev_frame_color_,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            VkClearColorValue black{};
            VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(init_cmd, prev_frame_color_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &clear_range);

            transition_image(init_cmd, prev_frame_color_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            vkEndCommandBuffer(init_cmd);

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &init_cmd;
            vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
            vkQueueWaitIdle(gpu_.graphics_queue());
            vkFreeCommandBuffers(vk, gpu_.command_pool(), 1, &init_cmd);
        }
    }
    // Gather scene: frustum cull, build render queue and indirect batches
    {
        GatherInput gi{};
        gi.scene = scene;
        gi.view = gatherer_.view_matrix();
        gi.projection = gatherer_.projection_matrix();
        gi.camera_position = gatherer_.camera_position();
        gi.frame_index = current_frame_;
        gi.shadow_pass = &shadow_pass_;
        gi.pool = &pool;
        gi.stats = &gpu_.render_stats();
        gi.animation_system = animation_system_;
        gatherer_.gather(gi);
    }

    // Flush any pending bindless texture/material updates
    gpu_.bindless_textures().update_descriptors();
    gpu_.bindless_textures().update_material_buffer();
    // Upload world-space AABBs for GPU frustum culling
    if (gpu_.use_mesh_shaders() && cull_pass_.enabled()) {
        uint32_t opaque_count = gatherer_.indirect_batch()->opaque_draw_count(current_frame_);
        const auto* aabbs = gatherer_.indirect_batch()->aabb_data(current_frame_);
        if (opaque_count > 0 && aabbs) {
            cull_pass_.upload_aabbs(aabbs, opaque_count, current_frame_);
        }
    }

    // Ensure meshlet instance buffers are large enough
    if (gpu_.use_mesh_shaders()) {
        uint32_t total_mi = gatherer_.indirect_batch()->total_meshlet_instances(current_frame_);
        if (total_mi > 0) {
            meshlet_instance_pass_.ensure_capacity(current_frame_, total_mi);
        }
    }

    // Update meshlet descriptor (set 3) if mesh shaders are available
    if (gpu_.meshlet_buffer() && gpu_.meshlet_buffer()->total_meshlets() > 0) {
        gpu_.meshlet_descriptor().update(*gpu_.meshlet_buffer(), *gpu_.mega_buffer(), current_frame_,
                                    meshlet_instance_pass_.visible_buffer(current_frame_),
                                    meshlet_instance_pass_.visible_buffer_size(current_frame_));
    }

    // Update sprite/tilemap SSBOs for this frame
    if (scene) {
        sprite_renderer_.update(*scene, gatherer_.projection_matrix() * gatherer_.view_matrix(),
                                gatherer_.camera_position(), current_frame_, playing_);
        tilemap_renderer_.update(*scene, gatherer_.projection_matrix() * gatherer_.view_matrix(),
                                  current_frame_);
    }

    FrameContext ctx{};
    ctx.cmd = cmd;
    ctx.target = &target;
    ctx.queue = &gatherer_.render_queue();
    ctx.camera_position = gatherer_.camera_position();
    ctx.global_descriptor_set = gpu_.global_descriptor_set(current_frame_);
    ctx.bindless_set = gpu_.bindless_textures().descriptor_set();
    ctx.draw_data_set = gatherer_.draw_data_descriptor().descriptor_set(current_frame_);
    ctx.debug_draw = debug_draw_.get();
    ctx.scene = scene;
    ctx.delta_time = dt;
    ctx.selected_id = selected_id;
    ctx.show_colliders = show_colliders_;
    ctx.frame_index = current_frame_;
    ctx.indirect_batch = gatherer_.indirect_batch();
    ctx.mega_buffer = gpu_.mega_buffer();
    ctx.use_mesh_shaders = gpu_.use_mesh_shaders();
    ctx.pipeline_cache = gpu_.pipeline_cache();
    if (gpu_.use_mesh_shaders()) {
        ctx.meshlet_set = gpu_.meshlet_descriptor().descriptor_set(current_frame_);
        ctx.gpu_driven_indirect = meshlet_instance_pass_.indirect_buffer(current_frame_);
        ctx.gpu_driven_segment_size = meshlet_instance_pass_.segment_size(current_frame_);
    }
    ctx.mr_view = target.mr_view();
    ctx.prev_frame_color_view = prev_frame_color_view_;
    ctx.pp_resources = pp;
    ctx.skip_shadows = skip_shadows;
    ctx.flip_tonemap_y = flip_tonemap_y;
    if (pp->hiz() && pp->hiz()->ready()) {
        ctx.hiz_view = pp->hiz()->hiz_sampled_view;
        ctx.hiz_sampler = hiz_pass_.sampler();
        ctx.hiz_mip_count = pp->hiz()->hiz_mip_count;
    }
    ctx.tile_light_ssbo = gpu_.tile_light_ssbo(current_frame_).handle();
    ctx.tile_light_ssbo_size = gpu_.tile_light_ssbo(current_frame_).size();
    ctx.target_pool = &target_pool_;

    // Store current frame context for pass build callbacks
    current_ctx_ = ctx;

    // Init screen color image on first use (or after resize) so the descriptor
    // at binding 8 doesn't point to an UNDEFINED-layout image.
    if (target.screen_color_needs_init()) {
        vk_utils::transition_image(cmd, target.screen_color_image(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        target.mark_screen_color_initialized();
    }

    // Execute the render pipeline (passes registered in register_passes())
    // Use the target's actual sample count (not the global default) so that
    // viewports with different MSAA levels (e.g. 1x preview) work correctly.
    render_pipeline_.execute(cmd, target, target.sample_count(), active_sources_,
                              pool, gpu_.secondary_pools(), current_frame_);

    // Copy scene color to prev_frame_color for next frame's SSR
    // Only for viewports that have SSR enabled (skip for camera preview etc.)
    if (needs_ssr_buffer && ssr_pass_.enabled() && prev_frame_color_ != VK_NULL_HANDLE) {
        auto ext = target.extent();

        // color is in SHADER_READ_ONLY (after bloom/tonemap read it) → TRANSFER_SRC
        transition_image(cmd, target.color_image(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        // prev_frame → TRANSFER_DST
        transition_image(cmd, prev_frame_color_,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {ext.width, ext.height, 1};
        vkCmdCopyImage(cmd,
            target.color_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            prev_frame_color_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region);

        // prev_frame → SHADER_READ_ONLY for next frame's SSR trace
        transition_image(cmd, prev_frame_color_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}

void VulkanContext::render_preview(const ViewportRenderRequest& req,
                                    const DirectionalLight& light, Scene* scene,
                                    float dt, uint32_t selected_id, ThreadPool& pool) {
    if (!req.target) return;

    auto vk = gpu_.device();
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = gpu_.command_pool();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    auto* pp = req.pp_resources ? req.pp_resources : default_pp_.get();

    // Ensure AO image is in a valid layout — after resize it may be UNDEFINED.
    // The preview doesn't run GTAO, but the color pass reads ao_texture from
    // the global descriptor set. Transition it here so the read is valid.
    auto* gtao_imgs = default_pp_->gtao();
    if (gtao_imgs && gtao_imgs->images_need_init) {
        std::array<VkImageMemoryBarrier2, 2> ao_barriers{};
        ao_barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ao_barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ao_barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ao_barriers[0].image = gtao_imgs->ao_image;
        ao_barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ao_barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        ao_barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

        ao_barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ao_barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ao_barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ao_barriers[1].image = gtao_imgs->blur_image;
        ao_barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ao_barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        ao_barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = ao_barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Update global descriptor bindings that may be stale after apply_resize
    {
        VkDescriptorImageInfo depth_desc{};
        depth_desc.sampler = gpu_.depth_sampler();
        depth_desc.imageView = req.target->depth_view();
        depth_desc.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo ao_desc{};
        ao_desc.sampler = gtao_pass_.ao_sampler();
        ao_desc.imageView = gtao_imgs ? gtao_imgs->ao_view : gtao_pass_.ao_view();
        ao_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = gpu_.global_descriptor_set(current_frame_);
        writes[0].dstBinding = 4;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &depth_desc;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = gpu_.global_descriptor_set(current_frame_);
        writes[1].dstBinding = 5;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &ao_desc;

        VkDescriptorImageInfo sc_desc{};
        sc_desc.sampler = tonemap_pass_.output_sampler();
        sc_desc.imageView = req.target->screen_color_view();
        sc_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet sc_write{};
        sc_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sc_write.dstSet = gpu_.global_descriptor_set(current_frame_);
        sc_write.dstBinding = 8;
        sc_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sc_write.descriptorCount = 1;
        sc_write.pImageInfo = &sc_desc;

        vkUpdateDescriptorSets(vk, 2, writes.data(), 0, nullptr);
        vkUpdateDescriptorSets(vk, 1, &sc_write, 0, nullptr);
    }

    update_uniform_buffer(current_frame_, req.view, req.projection,
                          req.camera_position, light, scene, req.target->extent());
    update_light_ssbo(current_frame_, scene);
    render_scene_to(cmd, *req.target, scene, dt, selected_id, pool, pp,
                    req.skip_shadows, req.flip_tonemap_y);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(gpu_.graphics_queue());

    vkFreeCommandBuffers(vk, gpu_.command_pool(), 1, &cmd);
}

void VulkanContext::record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                                          const std::vector<ViewportRenderRequest>& viewports,
                                          const DirectionalLight& light,
                                          Scene* scene, float dt,
                                          uint32_t selected_id, ThreadPool& pool,
                                          bool playing) {
    FJELL_PROFILE_SCOPE_N("record_command_buffer");
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("Failed to begin recording command buffer");
        return;
    }

    // Dispatch pending SDF compositing (recorded into the main command buffer, no stall)
    if (!pending_sdf_instances_.empty()) {
        scene_sdf_pass_.composite(cmd, pending_sdf_instances_);
        pending_sdf_instances_.clear();
    }

    // Update shadow cascades from the first viewport's camera
    // Use scene-node light if available, matching update_uniform_buffer behavior
    DirectionalLight shadow_light = light;
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
                shadow_light.direction = dl->direction;
                shadow_light.color = dl->color;
                shadow_light.intensity = dl->intensity;
                break;
            }
        }
    }
    if (!viewports.empty() && viewports[0].target) {
        shadow_pass_.update(viewports[0].view, viewports[0].projection,
                            shadow_light, 0.1f, 1000.0f);
    }

    // Scan for volumetric fog node once per frame (before viewports)
    volumetric_fog_pass_.enabled_ = false;
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
                volumetric_fog_pass_.enabled_ = true;
                volumetric_fog_pass_.density_ = fog->density;
                volumetric_fog_pass_.height_ = glm::vec3(fog->world_matrix_[3]).y;
                volumetric_fog_pass_.height_falloff_ = fog->height_falloff;
                volumetric_fog_pass_.scattering_ = fog->scattering;
                volumetric_fog_pass_.anisotropy_ = fog->anisotropy;
                volumetric_fog_pass_.max_distance_ = fog->max_distance;
                volumetric_fog_pass_.temporal_blend_ = fog->temporal_blend;
                volumetric_fog_pass_.fog_color_ = fog->fog_color;
                break;
            }
        }
    }

    // Scan for world environment node once per frame
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (auto* env = dynamic_cast<const WorldEnvironmentNode*>(obj.get())) {
                sky_pass_.set_top_color(env->sky_top_color);
                sky_pass_.set_bottom_color(env->sky_bottom_color);
                sky_cubemap_pass_.set_colors(env->sky_top_color, env->sky_bottom_color);
                ibl_intensity_ = env->ibl_intensity;
                break;
            }
        }
    }

    // --- Render scene once per viewport ---
    for (const auto& vp : viewports) {
        if (!vp.target) continue;
        auto* pp = vp.pp_resources ? vp.pp_resources : default_pp_.get();
        update_uniform_buffer(current_frame_, vp.view, vp.projection,
                              vp.camera_position, light, scene, vp.target->extent());
        update_light_ssbo(current_frame_, scene);
        render_scene_to(cmd, *vp.target, scene, dt, selected_id, pool, pp, vp.skip_shadows);
    }

    // --- UI editor preview offscreen pass ---
    if (ui_preview_callback_) {
        ui_preview_callback_(current_frame_);
    }

    // --- Render ImGui to swapchain ---
    {
        // Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
        transition_image(cmd, gpu_.swapchain().image(image_index),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = gpu_.swapchain().image_view(image_index);
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.012f, 0.012f, 0.015f, 1.0f}};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = {0, 0};
        rendering_info.renderArea.extent = gpu_.swapchain().extent();
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;

        vkCmdBeginRendering(cmd, &rendering_info);

        if (overlay_callback_) {
            overlay_callback_(cmd);
        }

        // Game UI renders to swapchain in play mode, after ImGui
        if (playing && game_ui_callback_) {
            game_ui_callback_(cmd, gpu_.global_descriptor_set(current_frame_),
                              gpu_.bindless_textures().descriptor_set(),
                              gpu_.swapchain().extent(), current_frame_);
        }

        vkCmdEndRendering(cmd);

        // Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
        transition_image(cmd, gpu_.swapchain().image(image_index),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }

#ifdef FJELL_ENABLE_TRACY
    TracyVkCollect(gpu_.tracy_ctx(), cmd);
#endif

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("Failed to end recording command buffer");
    }
}

void VulkanContext::update_uniform_buffer(uint32_t frame_index,
                                          const glm::mat4& view,
                                          const glm::mat4& projection,
                                          const glm::vec3& camera_position,
                                          const DirectionalLight& light,
                                          Scene* scene, VkExtent2D extent) {
    FJELL_PROFILE_SCOPE_N("update_uniform_buffer");
    gatherer_.set_camera_state(camera_position, view, projection);
    GlobalUbo ubo{};
    ubo.view = view;
    ubo.projection = projection;
    // Find directional light from scene nodes, fall back to passed-in light
    DirectionalLight dir_light = light;
    uint32_t point_count = 0;
    uint32_t spot_count = 0;
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
                dir_light.direction = dl->direction;
                dir_light.color = dl->color;
                dir_light.intensity = dl->intensity;
                dir_light.ambient_color = dl->ambient_color;
                dir_light.ambient_intensity = dl->ambient_intensity;
            } else if (dynamic_cast<const PointLightNode*>(obj.get())) {
                ++point_count;
            } else if (dynamic_cast<const SpotLightNode*>(obj.get())) {
                ++spot_count;
            }
        }
        // Also count legacy lights
        point_count += static_cast<uint32_t>(scene->point_lights().size());
        spot_count += static_cast<uint32_t>(scene->spot_lights().size());
    }
    point_count = std::min(point_count, 1024u);
    spot_count = std::min(spot_count, 1024u);

    ubo.light_direction = glm::vec4(glm::normalize(-dir_light.direction), dir_light.intensity);
    ubo.light_color = glm::vec4(dir_light.color, 0.0f);
    ubo.camera_position = glm::vec4(camera_position, 0.0f);
    ubo.ambient_color = glm::vec4(dir_light.ambient_color, dir_light.ambient_intensity * ibl_intensity_);

    const auto& matrices = shadow_pass_.light_matrices();
    const auto& splits = shadow_pass_.cascade_splits();
    for (uint32_t i = 0; i < ShadowPass::CASCADE_COUNT; ++i) {
        ubo.cascade_vp[i] = matrices[i];
    }
    ubo.cascade_splits = glm::vec4(splits[0], splits[1], splits[2], 0.0f);

    // Forward+ tile info
    uint32_t tiles_x = (extent.width + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t tiles_y = (extent.height + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    ubo.tile_info = glm::uvec4(tiles_x, tiles_y, point_count, spot_count);
    ubo.inv_projection = glm::inverse(projection);
    ubo.inv_view_proj = glm::inverse(projection * view);
    ubo.prev_view_proj = gatherer_.prev_view_proj();
    ubo.time = elapsed_time_;

    gpu_.uniform_buffer(frame_index).write(&ubo, sizeof(ubo));

    gatherer_.update_prev_view_proj();
}

void VulkanContext::update_light_ssbo(uint32_t frame_index, Scene* scene) {
    FJELL_PROFILE_SCOPE_N("update_light_ssbo");
    if (!scene) return;

    auto* dst = static_cast<uint8_t*>(gpu_.light_ssbo(frame_index).mapped());
    uint32_t point_count = 0;
    uint32_t spot_count = 0;

    // Gather lights from scene nodes (position from world transform)
    for (const auto& obj : scene->objects()) {
        if (auto* pl = dynamic_cast<const PointLightNode*>(obj.get())) {
            if (point_count >= 1024) continue;
            GpuPointLight gpu{};
            glm::vec3 pos = glm::vec3(pl->world_matrix_[3]);
            gpu.position_radius = glm::vec4(pos, pl->radius);
            gpu.color_intensity = glm::vec4(pl->color, pl->intensity);
            std::memcpy(dst + point_count * sizeof(GpuPointLight), &gpu, sizeof(GpuPointLight));
            ++point_count;
        } else if (auto* sl = dynamic_cast<const SpotLightNode*>(obj.get())) {
            if (spot_count >= 1024) continue;
            GpuSpotLight gpu{};
            glm::vec3 pos = glm::vec3(sl->world_matrix_[3]);
            gpu.position_radius = glm::vec4(pos, sl->radius);
            gpu.direction_inner_cos = glm::vec4(glm::normalize(sl->direction),
                                                 std::cos(glm::radians(sl->inner_angle)));
            gpu.color_intensity = glm::vec4(sl->color, sl->intensity);
            gpu.outer_cos = std::cos(glm::radians(sl->outer_angle));
            auto* spot_dst = dst + 1024 * sizeof(GpuPointLight);
            std::memcpy(spot_dst + spot_count * sizeof(GpuSpotLight), &gpu, sizeof(GpuSpotLight));
            ++spot_count;
        }
    }

    // Legacy: also include lights from the old separate vectors
    const auto& old_point_lights = scene->point_lights();
    for (uint32_t i = 0; i < static_cast<uint32_t>(old_point_lights.size()) && point_count < 1024; ++i) {
        const auto& pl = old_point_lights[i];
        GpuPointLight gpu{};
        gpu.position_radius = glm::vec4(pl.position, pl.radius);
        gpu.color_intensity = glm::vec4(pl.color, pl.intensity);
        std::memcpy(dst + point_count * sizeof(GpuPointLight), &gpu, sizeof(GpuPointLight));
        ++point_count;
    }

    const auto& old_spot_lights = scene->spot_lights();
    auto* spot_dst = dst + 1024 * sizeof(GpuPointLight);
    for (uint32_t i = 0; i < static_cast<uint32_t>(old_spot_lights.size()) && spot_count < 1024; ++i) {
        const auto& sl = old_spot_lights[i];
        GpuSpotLight gpu{};
        gpu.position_radius = glm::vec4(sl.position, sl.radius);
        gpu.direction_inner_cos = glm::vec4(glm::normalize(sl.direction),
                                             std::cos(glm::radians(sl.inner_angle)));
        gpu.color_intensity = glm::vec4(sl.color, sl.intensity);
        gpu.outer_cos = std::cos(glm::radians(sl.outer_angle));
        std::memcpy(spot_dst + spot_count * sizeof(GpuSpotLight), &gpu, sizeof(GpuSpotLight));
        ++spot_count;
    }
}

void VulkanContext::init_game_ui(GameUi& ui, const std::string& font_path,
                                VkFormat output_format) {
    PassResources res{};
    res.global_set_layout = gpu_.global_set_layout();
    res.material_set_layout = gpu_.bindless_textures().layout();
    res.draw_data_layout = gatherer_.draw_data_descriptor().layout();
    res.meshlet_layout = gpu_.use_mesh_shaders() ? gpu_.meshlet_descriptor().layout() : VK_NULL_HANDLE;
    res.depth_format = gpu_.device_ref().find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = gpu_.msaa_samples();

    ui.create(gpu_.device_ref(), gpu_.allocator(), res, gpu_.bindless_textures(),
              gpu_.command_pool(), gpu_.graphics_queue(),
              font_path, output_format);
}

void VulkanContext::init_game_ui_shared(GameUi& ui, const FontAtlas& shared_atlas,
                                         VkFormat output_format) {
    PassResources res{};
    res.global_set_layout = gpu_.global_set_layout();
    res.material_set_layout = gpu_.bindless_textures().layout();
    res.draw_data_layout = gatherer_.draw_data_descriptor().layout();
    res.meshlet_layout = gpu_.use_mesh_shaders() ? gpu_.meshlet_descriptor().layout() : VK_NULL_HANDLE;
    res.depth_format = gpu_.device_ref().find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = gpu_.msaa_samples();

    ui.create_with_shared_font(gpu_.device_ref(), gpu_.allocator(), res, gpu_.bindless_textures(),
                                shared_atlas, output_format);
}

void VulkanContext::init_game_ui_world(GameUi& ui, const FontAtlas& shared_atlas) {
    PassResources res{};
    res.global_set_layout = gpu_.global_set_layout();
    res.material_set_layout = gpu_.bindless_textures().layout();
    res.draw_data_layout = gatherer_.draw_data_descriptor().layout();
    res.meshlet_layout = gpu_.use_mesh_shaders() ? gpu_.meshlet_descriptor().layout() : VK_NULL_HANDLE;
    res.depth_format = gpu_.device_ref().find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = gpu_.msaa_samples();

    ui.create_with_shared_font(gpu_.device_ref(), gpu_.allocator(), res, gpu_.bindless_textures(),
                                shared_atlas, OffscreenPass::COLOR_FORMAT);
    ui.create_world_pipelines(gpu_.device_ref(), res);
}

void VulkanContext::record_game_ui_offscreen(GameUi& ui, OffscreenPass& target,
                                              uint32_t frame_index) {
    if (!ui.loaded()) return;

    VkCommandBuffer cmd = gpu_.command_buffer(frame_index);
    VkExtent2D extent = target.extent();

    // Transition offscreen color to color attachment
    VkImageMemoryBarrier to_attach{};
    to_attach.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_attach.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_attach.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_attach.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_attach.image = target.color_image();
    to_attach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_attach);

    // Begin dynamic rendering targeting offscreen color
    VkRenderingAttachmentInfo color_att{};
    color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView = target.msaa_color_view();
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    // Resolve attachment if MSAA
    if (target.sample_count() != VK_SAMPLE_COUNT_1_BIT) {
        color_att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_att.resolveImageView = target.color_view();
        color_att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_att;

    vkCmdBeginRendering(cmd, &ri);

    ui.record(cmd, gpu_.global_descriptor_set(frame_index),
              gpu_.bindless_textures().descriptor_set(),
              extent, frame_index);

    vkCmdEndRendering(cmd);

    // Transition to shader read for ImGui display
    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.image = target.color_image();
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_read);
}

void VulkanContext::submit_and_present() {
    if (!has_recorded_frame_) return;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    auto& render_finished = gpu_.swapchain().render_finished_semaphores();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_semaphore = gpu_.image_available_semaphore(recorded_frame_);
    VkCommandBuffer cmd_buf = gpu_.command_buffer(recorded_frame_);
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &wait_semaphore;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished[recorded_image_index_];

    VkResult submit_result = vkQueueSubmit(gpu_.graphics_queue(), 1, &submit_info, gpu_.in_flight_fence(recorded_frame_));
    if (submit_result != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("vkQueueSubmit failed: {}", static_cast<int>(submit_result));
        has_recorded_frame_ = false;
        return;
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished[recorded_image_index_];
    present_info.swapchainCount = 1;
    VkSwapchainKHR swapchain_handle = gpu_.swapchain().handle();
    present_info.pSwapchains = &swapchain_handle;
    present_info.pImageIndices = &recorded_image_index_;

    VkResult result = vkQueuePresentKHR(gpu_.device_ref().present_queue(), &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        gpu_.window().was_resized()) {
        gpu_.window().reset_resized();
        gpu_.swapchain().recreate();
    } else if (result != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("vkQueuePresentKHR failed: {}", static_cast<int>(result));
    }

    target_pool_.end_frame();
    has_recorded_frame_ = false;
}

bool VulkanContext::begin_frame(ThreadPool& pool) {
    FJELL_PROFILE_SCOPE_N("begin_frame");
    auto vk_device = gpu_.device();

    // Advance to next frame slot
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;

    VkFence frame_fence = gpu_.in_flight_fence(current_frame_);
    vkWaitForFences(vk_device, 1, &frame_fence, VK_TRUE,
                    std::numeric_limits<uint64_t>::max());

    // GPU is now done with this frame slot — safe to destroy/resize resources.

    // Lazy-init per-thread command pools (needs thread count from pool)
    gpu_.ensure_secondary_pools(pool.thread_count());
    if (pool.thread_count() > 0) {
        gpu_.secondary_pools().reset_frame(current_frame_);
    }

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(vk_device, gpu_.swapchain().handle(),
                          std::numeric_limits<uint64_t>::max(),
                          gpu_.image_available_semaphore(current_frame_), VK_NULL_HANDLE,
                          &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_NOT_READY) {
        gpu_.swapchain().recreate();
        has_recorded_frame_ = false;
        return false;  // fence stays signaled — next wait will pass through
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        FJELL_GFX_CRITICAL("vkAcquireNextImageKHR failed: {}", static_cast<int>(result));
        has_recorded_frame_ = false;
        return false;
    }

    // Reset fence only after successful acquire — ensures the fence stays
    // signaled on the error/recreate path so the next vkWaitForFences doesn't hang
    vkResetFences(vk_device, 1, &frame_fence);
    pending_image_index_ = image_index;
    return true;
}

void VulkanContext::record_frame(const std::vector<ViewportRenderRequest>& viewports,
                                 const DirectionalLight& light,
                                 Scene* scene, float dt,
                                 uint32_t selected_id, ThreadPool& pool,
                                 bool playing) {
    FJELL_PROFILE_SCOPE_N("record_frame");
    auto vk_device = gpu_.device();
    elapsed_time_ += dt;

    gpu_.render_stats().reset();
    if (scene && (show_colliders_ || selected_id != 0)) {
        debug_draw_->update(*scene, selected_id, show_colliders_);
    }

    // Sync particle emitters — may rebuild GPU buffers.
    // Safe here: begin_frame() already waited on this frame slot's fence,
    // so the GPU is done with this slot's resources.
    if (scene) {
        particle_system_.sync_emitters(*scene);
    }

    // Update scene SDF — register mesh SDFs and check dirty transforms
    {
        std::vector<GpuSdfInstance> sdf_instances;
        auto gather_sdf = [&](Scene& s) {
            for (const auto& obj : s.objects()) {
                auto* spatial = obj->as_spatial();
                if (!spatial) continue;
                auto* mesh_comp = obj->components.get<MeshComponent>();
                if (!mesh_comp || !mesh_comp->model) continue;

                auto* model = mesh_comp->model;
                if (!model->has_sdf()) continue;

                scene_sdf_pass_.register_model_sdf(*model, reinterpret_cast<uintptr_t>(model));

                if (spatial->world_dirty_) {
                    scene_sdf_pass_.mark_dirty();
                }

                auto it = scene_sdf_pass_.model_entry(reinterpret_cast<uintptr_t>(model));
                if (it) {
                    GpuSdfInstance inst{};
                    inst.world_to_sdf = glm::inverse(spatial->world_matrix_);
                    inst.sdf_min = it->sdf_min;
                    inst.sdf_scale = it->max_dist;
                    inst.sdf_max = it->sdf_max;
                    inst.grid_offset = it->offset;
                    inst.grid_resolution = glm::uvec3(it->resolution);
                    sdf_instances.push_back(inst);
                }
            }
        };

        if (scene) gather_sdf(*scene);

        // Gather from active editor context's preview scene (e.g. VFX editor ground plane)
        if (active_render_scene_ && active_render_scene_ != scene) {
            gather_sdf(*active_render_scene_);
        }

        if (scene_sdf_pass_.dirty() && !sdf_instances.empty()) {
            pending_sdf_instances_ = std::move(sdf_instances);
        }
    }

    // Update binding 4 (depth texture for light culling) before recording
    // Must happen before command buffer starts to avoid invalidating in-flight descriptors
    if (!viewports.empty() && viewports[0].target) {
        VkDescriptorImageInfo depth_desc{};
        depth_desc.sampler = gpu_.depth_sampler();
        depth_desc.imageView = viewports[0].target->depth_view();
        depth_desc.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet depth_write{};
        depth_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        depth_write.dstSet = gpu_.global_descriptor_set(current_frame_);
        depth_write.dstBinding = 4;
        depth_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depth_write.descriptorCount = 1;
        depth_write.pImageInfo = &depth_desc;
        vkUpdateDescriptorSets(vk_device, 1, &depth_write, 0, nullptr);

        // Update binding 5 (AO texture for ambient occlusion)
        VkDescriptorImageInfo ao_desc{};
        ao_desc.sampler = gtao_pass_.ao_sampler();
        ao_desc.imageView = default_pp_->gtao() ? default_pp_->gtao()->ao_view : gtao_pass_.ao_view();
        ao_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet ao_write{};
        ao_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ao_write.dstSet = gpu_.global_descriptor_set(current_frame_);
        ao_write.dstBinding = 5;
        ao_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ao_write.descriptorCount = 1;
        ao_write.pImageInfo = &ao_desc;
        vkUpdateDescriptorSets(vk_device, 1, &ao_write, 0, nullptr);

        // Update binding 8 (screen color for SCREEN_TEXTURE in transparent shaders)
        VkDescriptorImageInfo sc_desc{};
        sc_desc.sampler = tonemap_pass_.output_sampler();
        sc_desc.imageView = viewports[0].target->screen_color_view();
        sc_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet sc_write{};
        sc_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sc_write.dstSet = gpu_.global_descriptor_set(current_frame_);
        sc_write.dstBinding = 8;
        sc_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sc_write.descriptorCount = 1;
        sc_write.pImageInfo = &sc_desc;
        vkUpdateDescriptorSets(vk_device, 1, &sc_write, 0, nullptr);
    }

    vkResetCommandBuffer(gpu_.command_buffer(current_frame_), 0);
    record_command_buffer(gpu_.command_buffer(current_frame_), pending_image_index_,
                          viewports, light, scene, dt, selected_id, pool, playing);

    recorded_frame_ = current_frame_;
    recorded_image_index_ = pending_image_index_;
    has_recorded_frame_ = true;
}

void VulkanContext::wait_idle() {
    vkDeviceWaitIdle(gpu_.device());
}

void VulkanContext::release_imgui_textures() {
    default_pp_->invalidate_imgui_textures();
    depth_viz_pass_.invalidate_imgui_texture();
    wireframe_pass_.invalidate_imgui_texture();
    normal_pass_.invalidate_imgui_texture();
    overdraw_pass_.invalidate_imgui_texture();
    uv_viz_pass_.invalidate_imgui_texture();
    meshlet_viz_pass_.invalidate_imgui_texture();
    cull_viz_pass_.invalidate_imgui_texture();
    light_heatmap_pass_.invalidate_imgui_texture();
    light_grid_pass_.invalidate_imgui_texture();
}

void VulkanContext::draw_outline(VkCommandBuffer cmd, const FrameContext& ctx) {
    if (ctx.selected_id == 0 || !ctx.scene) return;

    const auto* obj = ctx.scene->find(ctx.selected_id);
    if (!obj) return;
    const auto* spatial = obj->as_spatial();
    if (!spatial) return;
    const auto* mesh = obj->components.get<MeshComponent>();
    if (!mesh || !mesh->model || !mesh->material) return;

    auto* cache = gpu_.pipeline_cache();
    uint32_t pid = mesh->material->pipeline_id();
    auto pipeline = cache->outline_pipeline(pid);
    auto layout = cache->outline_layout();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Bind all 3 descriptor sets (global + material + draw_data)
    std::array descriptor_sets = {ctx.global_descriptor_set,
                                   gpu_.bindless_textures().descriptor_set(),
                                   gatherer_.draw_data_descriptor().descriptor_set(current_frame_)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, static_cast<uint32_t>(descriptor_sets.size()),
                            descriptor_sets.data(), 0, nullptr);

    VkViewport vp{};
    vp.width = static_cast<float>(ctx.target->extent().width);
    vp.height = static_cast<float>(ctx.target->extent().height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.target->extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Push: outline_color vec4 + thickness float
    struct {
        glm::vec4 color;
        float thickness;
    } push{};
    push.color = {1.0f, 0.6f, 0.15f, 0.7f};
    push.thickness = 0.003f;

    vkCmdPushConstants(cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    // Find the draw index for this object in the draw data SSBO
    const auto& queue = gatherer_.render_queue();
    uint32_t draw_index = 0;
    bool found = false;
    for (const auto& dc : queue.commands()) {
        if (dc.node_id == ctx.selected_id) { found = true; break; }
        draw_index++;
    }
    if (!found) return;

    // Draw with firstInstance = draw_index so gl_BaseInstance reads the right DrawData
    gpu_.mega_buffer()->bind(cmd);
    mesh->model->draw(cmd, draw_index);
}

void VulkanContext::update_shadow_descriptors() {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo shadow_info{};
        shadow_info.sampler = shadow_pass_.sampler();
        shadow_info.imageView = shadow_pass_.array_view();
        shadow_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = gpu_.global_descriptor_set(i);
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &shadow_info;

        vkUpdateDescriptorSets(gpu_.device(), 1, &write, 0, nullptr);
    }
}

void VulkanContext::draw_sdf_debug_window() {
    if (!show_sdf_debug_) return;

    if (ImGui::Begin("SDF Debug", &show_sdf_debug_)) {
        const char* axes[] = {"X", "Y", "Z"};
        ImGui::Combo("Axis", &sdf_debug_axis_, axes, 3);
        ImGui::SliderFloat("Slice", &sdf_debug_slice_, 0.0f, 1.0f);

        glm::vec3 vol_min = scene_sdf_pass_.volume_min();
        glm::vec3 vol_max = scene_sdf_pass_.volume_max();
        bool bounds_changed = false;
        if (ImGui::DragFloat3("Vol Min", &vol_min.x, 0.5f)) bounds_changed = true;
        if (ImGui::DragFloat3("Vol Max", &vol_max.x, 0.5f)) bounds_changed = true;
        if (bounds_changed) scene_sdf_pass_.set_bounds(vol_min, vol_max);

        ImGui::Text("Resolution: %u", scene_sdf_pass_.resolution());

        // Dispatch slice render
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = gpu_.command_pool();
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(gpu_.device(), &alloc, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        scene_sdf_pass_.render_slice(cmd, static_cast<uint32_t>(sdf_debug_axis_), sdf_debug_slice_);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(gpu_.graphics_queue());
        vkFreeCommandBuffers(gpu_.device(), gpu_.command_pool(), 1, &cmd);

        // Display slice image
        auto imgui_set = scene_sdf_pass_.slice_imgui_set();
        if (imgui_set) {
            float avail = ImGui::GetContentRegionAvail().x;
            float size = std::min(avail, 256.0f);
            ImGui::Image(imgui_set, {size, size});
        }
    }
    ImGui::End();
}

void VulkanContext::reload_pipeline() {
    gpu_.pipeline()->recreate();
    gpu_.pipeline_cache()->recreate_all();
    particle_system_.recreate_pipelines();
    sync_custom_depth_pipelines();
}

void VulkanContext::sync_custom_depth_pipelines() {
    auto* pc = gpu_.pipeline_cache();
    if (!pc) return;

    for (uint32_t id = 0; id < pc->shader_count(); ++id) {
        if (!pc->has_custom_vertex(id)) continue;

        if (gpu_.mesh_shaders_available()) {
            const auto& depth_mesh = pc->depth_mesh_spv(id);
            if (!depth_mesh.empty()) {
                mesh_shader_depth_pass_.register_custom_depth(id, depth_mesh);
            }
        }

        const auto& depth_vert = pc->depth_vert_spv(id);
        if (!depth_vert.empty()) {
            depth_prepass_.register_custom_depth(id, depth_vert);
        }

        const auto& shadow_vert = pc->shadow_vert_spv(id);
        if (!shadow_vert.empty()) {
            shadow_pass_.register_custom_shadow(id, shadow_vert);
        }
    }
}

// ── Scene Viewports ──────────────────────────────────────────────────────

uint32_t VulkanContext::create_scene_viewport(const SceneViewportConfig& config) {
    uint32_t bindless_idx = gpu_.bindless_textures().reserve_index();

    PostProcessResources::PassRefs refs{
        hiz_pass_, gtao_pass_, bloom_pass_,
        ssr_pass_, volumetric_fog_pass_, tonemap_pass_};

    auto viewport = std::make_unique<SceneViewport>(
        gpu_.device_ref(), gpu_.device(), gpu_.allocator(),
        config, refs, bindless_idx);

    scene_viewports_[config.name] = std::move(viewport);
    return bindless_idx;
}

void VulkanContext::destroy_scene_viewport(const std::string& name) {
    auto it = scene_viewports_.find(name);
    if (it == scene_viewports_.end()) return;
    wait_idle();
    scene_viewports_.erase(it);
}

SceneViewport* VulkanContext::scene_viewport(const std::string& name) {
    auto it = scene_viewports_.find(name);
    return it != scene_viewports_.end() ? it->second.get() : nullptr;
}

uint32_t VulkanContext::scene_viewport_bindless_index(const std::string& name) const {
    auto it = scene_viewports_.find(name);
    return it != scene_viewports_.end() ? it->second->bindless_index() : 0;
}

void VulkanContext::render_scene_viewports(Scene* scene, float dt, uint32_t selected_id,
                                            const DirectionalLight& light, ThreadPool& pool) {
    for (auto& [name, viewport] : scene_viewports_) {
        render_preview(viewport->build_request(), light, scene, dt, selected_id, pool);
        viewport->sync_bindless(gpu_.bindless_textures());
    }
}

} // namespace fjell
