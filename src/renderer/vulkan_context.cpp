#include "renderer/vulkan_context.hpp"
#include "renderer/pass_setup.hpp"
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

    // Finalize global descriptor layout/pool/sets (builtins already declared
    // by GpuContext constructor; additional bindings could be declared here)
    gpu_.finalize_descriptors();

    // Scene gathering: indirect batches + draw data descriptors
    gatherer_.init(gpu_);

    // Init pipelines now that draw_data_layout is available
    gpu_.init_pipelines(gatherer_.draw_data_descriptor().layout());

    // Build PassResources with draw_data_layout
    PassResources pass_resources = gpu_.pass_resources();
    pass_resources.draw_data_layout = gatherer_.draw_data_descriptor().layout();

    // Create all render passes (centralized in pass_setup.cpp)
    create_all_passes(passes_, gpu_, pass_resources);
    meshlet_passes_created_ = passes_.has("meshlet_instance");

    // Create acceleration structure manager for DDGI RT backend
    if (gpu_.device_ref().ray_tracing_supported()) {
        accel_manager_ = std::make_unique<AccelStructureManager>(
            gpu_.device_ref(), gpu_.allocator(),
            gpu_.command_pool(), gpu_.graphics_queue());
        accel_manager_->create_particle_tlas_pipeline(gpu_.vk_pipeline_cache());
        ddgi_pass().set_accel_manager(accel_manager_.get());
    }

    // Create render subsystems
    auto add_subsystem = [&](auto sys) {
        sys->create(gpu_.device_ref(), pass_resources);
        render_subsystems_.push_back(std::move(sys));
    };

    {
        auto ps = std::make_unique<ParticleSystem>();
        particle_system_ = ps.get();
        ddgi_pass().set_particle_system(particle_system_);
        add_subsystem(std::move(ps));
    }
    {
        auto sr = std::make_unique<SpriteRenderer>();
        sprite_renderer_ = sr.get();
        add_subsystem(std::move(sr));
    }
    {
        auto tr = std::make_unique<TilemapRenderer>();
        tilemap_renderer_ = tr.get();
        add_subsystem(std::move(tr));
    }
    // Sort subsystems by transparent draw order (lower = drawn first = behind)
    std::sort(render_subsystems_.begin(), render_subsystems_.end(),
              [](const auto& a, const auto& b) {
                  return a->transparent_order() < b->transparent_order();
              });

    // Cache raw pointers for FrameContext
    subsystem_ptrs_.reserve(render_subsystems_.size());
    for (auto& s : render_subsystems_) {
        subsystem_ptrs_.push_back(s.get());
    }

    FJELL_GFX_DEBUG("Outline pipelines managed by PipelineCache");
    FJELL_GFX_DEBUG("Creating debug draw");
    debug_draw_ = std::make_unique<DebugDraw>(gpu_.device(), gpu_.allocator());

    register_passes();

    // Default post-process resources for the main viewport (all passes enabled)
    PostProcessConfig default_config{};
    default_config.ssr = true;
    default_config.volumetric_fog = true;
    default_config.ddgi = true;
    PostProcessResources::PassRefs refs{hiz_pass(), gtao_pass(), bloom_pass(), ssr_pass(), volumetric_fog_pass(), tonemap_pass(), ddgi_pass()};
    default_pp_ = std::make_unique<PostProcessResources>(gpu_.device(), gpu_.allocator(), 256, 256, default_config, refs);

    FJELL_GFX_INFO("Vulkan context ready");
}

VulkanContext::~VulkanContext() {
    default_pp_.reset();
    debug_draw_.reset();
    for (auto& sys : render_subsystems_) sys->destroy();
    render_subsystems_.clear();
    particle_system_ = nullptr;
    sprite_renderer_ = nullptr;
    tilemap_renderer_ = nullptr;
    accel_manager_.reset();

    passes_.destroy_all();
}

void VulkanContext::set_use_mesh_shaders(bool enable) {
    gpu_.set_use_mesh_shaders(enable);

    // Lazily create meshlet passes on first enable
    if (enable && gpu_.use_mesh_shaders() && !meshlet_passes_created_) {
        PassResources pass_resources = gpu_.pass_resources();
        pass_resources.draw_data_layout = gatherer_.draw_data_descriptor().layout();

        if (!create_meshlet_passes(passes_, gpu_, pass_resources)) {
            FJELL_GFX_ERROR("Failed to create meshlet passes — disabling mesh shaders");
            gpu_.set_use_mesh_shaders(false);
            return;
        }
        meshlet_passes_created_ = true;

        // Register new passes with the render pipeline
        for (const auto& name : {"meshlet_instance", "mesh_shader_depth",
                                  "meshlet_viz", "cull", "cull_viz"}) {
            passes_.register_pass_with_pipeline(name, render_pipeline_, current_ctx_);
        }

        // Register custom vertex displacement depth pipelines for the newly
        // created mesh shader depth pass (vertex prepass + shadow already have theirs).
        auto* pc = gpu_.pipeline_cache();
        if (pc) {
            for (uint32_t id = 0; id < pc->shader_count(); ++id) {
                if (!pc->has_custom_vertex(id)) continue;
                const auto& depth_mesh = pc->depth_mesh_spv(id);
                if (!depth_mesh.empty()) {
                    mesh_shader_depth_pass()->register_custom_depth(id, depth_mesh);
                }
            }
        }

        FJELL_GFX_INFO("Meshlet passes created (lazy init)");
    }
}

void VulkanContext::sync_sprite_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    sprite_renderer_->sync_textures(scene, resources, project);
}

void VulkanContext::sync_tilemap_textures(Scene& scene, ResourceManager& resources, const Project* project) {
    tilemap_renderer_->sync_textures(scene, resources, project);
}

// ── Pass registration ────────────────────────────────────────────────────

void VulkanContext::register_passes() {
    passes_.register_with_pipeline(render_pipeline_, current_ctx_);
}

// ── Command buffer recording ────────────────────────────────────────────

FrameContext VulkanContext::build_frame_context(VkCommandBuffer cmd, OffscreenPass& target,
                                                 Scene* scene, float dt, uint32_t selected_id,
                                                 PostProcessResources* pp, bool skip_shadows,
                                                 bool flip_tonemap_y) {
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
    if (gpu_.use_mesh_shaders() && passes_.has("meshlet_instance")) {
        ctx.meshlet_set = gpu_.meshlet_descriptor().descriptor_set(current_frame_);
        ctx.gpu_driven_indirect = meshlet_instance_pass()->indirect_buffer(current_frame_);
        ctx.gpu_driven_segment_size = meshlet_instance_pass()->segment_size(current_frame_);
    }
    ctx.pp_resources = pp;
    ctx.registry = &resource_registry_;
    ctx.skip_shadows = skip_shadows;
    ctx.flip_tonemap_y = flip_tonemap_y;
    ctx.target_pool = &target_pool_;

    // Read inv_view_proj from the already-written GlobalUbo
    auto* ubo_data = static_cast<const GlobalUbo*>(gpu_.uniform_buffer(current_frame_).mapped());
    ctx.inv_view_proj = ubo_data->inv_view_proj;

    // Extended context for self-registering passes
    ctx.render_stats = &gpu_.render_stats();
#ifdef FJELL_ENABLE_TRACY
    ctx.tracy_ctx = gpu_.tracy_ctx();
#endif
    if (gpu_.use_mesh_shaders()) {
        ctx.draw_mesh_tasks_indirect_fn = gpu_.device_ref().draw_mesh_tasks_indirect_fn();
    }
    ctx.subsystems = subsystem_ptrs_.data();
    ctx.subsystem_count = static_cast<uint32_t>(subsystem_ptrs_.size());
    ctx.active_sources = active_sources_;
    ctx.passes = &passes_;
    ctx.playing = playing_;
    ctx.world_ui_callback = world_ui_callback_;
    ctx.game_ui_callback = game_ui_callback_;

    // Shadow cascade batches (filled from gatherer, consumed by ShadowPass::build)
    for (uint32_t c = 0; c < FrameContext::MAX_SHADOW_CASCADES; ++c) {
        ctx.shadow_cascade_batches[c] = gatherer_.shadow_batch(c);
        ctx.shadow_cascade_draw_data[c] =
            gatherer_.shadow_draw_data(c).descriptor_set(current_frame_);
        ctx.shadow_cascade_commands[c] = &gatherer_.shadow_cmds(c);
    }

    return ctx;
}

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
        ssr_pass().ensure_prev_frame(gpu_.device(), gpu_.allocator(),
                                     gpu_.command_pool(), gpu_.graphics_queue(),
                                     ext.width, ext.height);
    }
    // Gather scene: frustum cull, build render queue and indirect batches
    {
        GatherInput gi{};
        gi.scene = scene;
        gi.view = gatherer_.view_matrix();
        gi.projection = gatherer_.projection_matrix();
        gi.camera_position = gatherer_.camera_position();
        gi.frame_index = current_frame_;
        gi.shadow_pass = &shadow_pass();
        gi.pool = &pool;
        gi.stats = &gpu_.render_stats();
        gi.animation_system = animation_system_;
        gatherer_.gather(gi);
    }

    // Flush any pending bindless texture/material updates
    gpu_.bindless_textures().update_descriptors();
    gpu_.bindless_textures().update_material_buffer();
    // Upload world-space AABBs for GPU frustum culling
    bool has_meshlet_passes = passes_.has("meshlet_instance");
    if (gpu_.use_mesh_shaders() && has_meshlet_passes && cull_pass()->enabled()) {
        uint32_t opaque_count = gatherer_.indirect_batch()->opaque_draw_count(current_frame_);
        const auto* aabbs = gatherer_.indirect_batch()->aabb_data(current_frame_);
        if (opaque_count > 0 && aabbs) {
            cull_pass()->upload_aabbs(aabbs, opaque_count, current_frame_);
        }
    }

    // Ensure meshlet instance buffers are large enough
    if (gpu_.use_mesh_shaders() && has_meshlet_passes) {
        uint32_t total_mi = gatherer_.indirect_batch()->total_meshlet_instances(current_frame_);
        if (total_mi > 0) {
            meshlet_instance_pass()->ensure_capacity(current_frame_, total_mi);
        }
    }

    // Update meshlet descriptor (set 3) if mesh shaders are available
    if (has_meshlet_passes && gpu_.meshlet_buffer() && gpu_.meshlet_buffer()->total_meshlets() > 0) {
        gpu_.meshlet_descriptor().update(*gpu_.meshlet_buffer(), *gpu_.mega_buffer(), current_frame_,
                                    meshlet_instance_pass()->visible_buffer(current_frame_),
                                    meshlet_instance_pass()->visible_buffer_size(current_frame_));
    }

    // Update render subsystems (gather data, upload SSBOs)
    {
        RenderSubsystemContext sub_ctx{};
        sub_ctx.scene = scene;
        sub_ctx.resource_manager = resource_manager_;
        sub_ctx.project = project_;
        sub_ctx.view_projection = gatherer_.projection_matrix() * gatherer_.view_matrix();
        sub_ctx.camera_position = gatherer_.camera_position();
        sub_ctx.frame_index = current_frame_;
        sub_ctx.dt = dt;
        sub_ctx.playing = playing_;
        for (auto& sys : render_subsystems_) sys->update(sub_ctx);
    }

    current_ctx_ = build_frame_context(cmd, target, scene, dt, selected_id,
                                        pp, skip_shadows, flip_tonemap_y);

    // Build acceleration structures before command buffer recording starts.
    // BLAS builds use synchronous GPU submits that would conflict with in-flight recording.
    // Only needed when DDGI is enabled for this viewport — preview viewports skip this.
    if (pp && pp->ddgi()) {
        ddgi_pass().prepare_accel_structures(current_ctx_);
    }

    // Init screen color image on first use (or after resize) so the descriptor
    // at binding 8 doesn't point to an UNDEFINED-layout image.
    if (target.screen_color_needs_init()) {
        vk_utils::transition_image(cmd, target.screen_color_image(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        target.mark_screen_color_initialized();
    }

    // Update per-frame pass state before pipeline execution
    light_cull_pass().set_output_buffer(
        gpu_.tile_light_ssbo(current_frame_).handle(),
        gpu_.tile_light_ssbo(current_frame_).size());

    // Populate resource registry — passes self-register their outputs,
    // then non-pass resources (meshlet bounds) are added manually.
    resource_registry_.clear();
    passes_.register_resources_all(resource_registry_);
    if (gpu_.meshlet_buffer()) {
        resource_registry_.register_buffer("meshlet_bounds", {
            gpu_.meshlet_buffer()->bounds_buffer(),
            gpu_.meshlet_buffer()->bounds_buffer_size()});
    }

    render_pipeline_.execute(cmd, target, target.sample_count(), active_sources_,
                              pool, gpu_.secondary_pools(), current_frame_,
                              resource_registry_);

    // Copy scene color to prev_frame_color for next frame's SSR
    if (needs_ssr_buffer) {
        ssr_pass().copy_prev_frame(cmd, target.color_image(), target.extent());
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
        auto& gb = gpu_.global_bindings();
        gb.write_image(current_frame_, "depth_texture",
            req.target->depth_view(), gpu_.depth_sampler(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        VkImageView ao_view = gtao_imgs ? gtao_imgs->ao_view : gtao_pass().ao_view();
        gb.write_image(current_frame_, "ao_texture",
            ao_view, gtao_pass().ao_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gb.write_image(current_frame_, "screen_color",
            req.target->screen_color_view(), tonemap_pass().output_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // GI indirect: use fallback for preview viewports
        gb.write_image(current_frame_, "gi_indirect",
            gpu_.gi_fallback_view(), gpu_.gi_fallback_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gb.flush_writes();
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
        scene_sdf_pass().composite(cmd, pending_sdf_instances_);
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
        shadow_pass().update(viewports[0].view, viewports[0].projection,
                            shadow_light, 0.1f, 1000.0f);
    }

    // Scan for volumetric fog node once per frame (before viewports)
    volumetric_fog_pass().enabled_ = false;
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
                volumetric_fog_pass().enabled_ = true;
                volumetric_fog_pass().density_ = fog->density;
                volumetric_fog_pass().height_ = glm::vec3(fog->world_matrix_[3]).y;
                volumetric_fog_pass().height_falloff_ = fog->height_falloff;
                volumetric_fog_pass().scattering_ = fog->scattering;
                volumetric_fog_pass().anisotropy_ = fog->anisotropy;
                volumetric_fog_pass().max_distance_ = fog->max_distance;
                volumetric_fog_pass().temporal_blend_ = fog->temporal_blend;
                volumetric_fog_pass().fog_color_ = fog->fog_color;
                break;
            }
        }
    }

    // Scan for world environment node once per frame
    if (scene) {
        for (const auto& obj : scene->objects()) {
            if (auto* env = dynamic_cast<const WorldEnvironmentNode*>(obj.get())) {
                sky_pass().set_top_color(env->sky_top_color);
                sky_pass().set_bottom_color(env->sky_bottom_color);
                sky_cubemap_pass().set_colors(env->sky_top_color, env->sky_bottom_color);
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

    const auto& matrices = shadow_pass().light_matrices();
    const auto& splits = shadow_pass().cascade_splits();
    for (uint32_t i = 0; i < ShadowPass::CASCADE_COUNT; ++i) {
        ubo.cascade_vp[i] = matrices[i];
    }
    float gi_int = (default_pp_ && default_pp_->config().ddgi) ? ddgi_pass().config().gi_intensity : 0.0f;
    ubo.cascade_splits = glm::vec4(splits[0], splits[1], splits[2], gi_int);

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
    apply_pending_ddgi_toggle();

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
    elapsed_time_ += dt;

    gpu_.render_stats().reset();
    if (scene && (show_colliders_ || selected_id != 0)) {
        debug_draw_->update(*scene, selected_id, show_colliders_);
    }

    // Sync render subsystems — may rebuild GPU buffers.
    // Safe here: begin_frame() already waited on this frame slot's fence,
    // so the GPU is done with this slot's resources.
    {
        RenderSubsystemContext sub_ctx{};
        sub_ctx.scene = scene;
        sub_ctx.resource_manager = resource_manager_;
        sub_ctx.project = project_;
        sub_ctx.frame_index = current_frame_;
        for (auto& sys : render_subsystems_) sys->sync(sub_ctx);
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

                scene_sdf_pass().register_model_sdf(*model, reinterpret_cast<uintptr_t>(model));

                if (spatial->world_dirty_) {
                    scene_sdf_pass().mark_dirty();
                }

                auto it = scene_sdf_pass().model_entry(reinterpret_cast<uintptr_t>(model));
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

        if (scene_sdf_pass().dirty() && !sdf_instances.empty()) {
            pending_sdf_instances_ = std::move(sdf_instances);
        }
    }

    // Update per-frame global bindings before recording
    if (!viewports.empty() && viewports[0].target) {
        auto& gb = gpu_.global_bindings();

        gb.write_image(current_frame_, "depth_texture",
            viewports[0].target->depth_view(), gpu_.depth_sampler(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        VkImageView ao_view = default_pp_->gtao() ? default_pp_->gtao()->ao_view : gtao_pass().ao_view();
        gb.write_image(current_frame_, "ao_texture",
            ao_view, gtao_pass().ao_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        gb.write_image(current_frame_, "screen_color",
            viewports[0].target->screen_color_view(), tonemap_pass().output_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // GI indirect: use real view only after DDGI has run at least once
        auto* ddgi_imgs = default_pp_->ddgi();
        bool ddgi_ready = ddgi_imgs && ddgi_imgs->indirect_view != VK_NULL_HANDLE
                       && !ddgi_imgs->images_need_init;
        gb.write_image(current_frame_, "gi_indirect",
            ddgi_ready ? ddgi_imgs->indirect_view : gpu_.gi_fallback_view(),
            gpu_.gi_fallback_sampler(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        gb.flush_writes();
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
    depth_viz_pass().invalidate_imgui_texture();
    wireframe_pass().invalidate_imgui_texture();
    normal_pass().invalidate_imgui_texture();
    overdraw_pass().invalidate_imgui_texture();
    uv_viz_pass().invalidate_imgui_texture();
    if (auto* mv = passes_.try_get<MeshletVizPass>("meshlet_viz")) mv->invalidate_imgui_texture();
    if (auto* cv = passes_.try_get<CullVizPass>("cull_viz")) cv->invalidate_imgui_texture();
    light_heatmap_pass().invalidate_imgui_texture();
    light_grid_pass().invalidate_imgui_texture();
}

void VulkanContext::draw_sdf_debug_window() {
    if (!show_sdf_debug_) return;

    if (ImGui::Begin("SDF Debug", &show_sdf_debug_)) {
        const char* axes[] = {"X", "Y", "Z"};
        ImGui::Combo("Axis", &sdf_debug_axis_, axes, 3);
        ImGui::SliderFloat("Slice", &sdf_debug_slice_, 0.0f, 1.0f);

        glm::vec3 vol_min = scene_sdf_pass().volume_min();
        glm::vec3 vol_max = scene_sdf_pass().volume_max();
        bool bounds_changed = false;
        if (ImGui::DragFloat3("Vol Min", &vol_min.x, 0.5f)) bounds_changed = true;
        if (ImGui::DragFloat3("Vol Max", &vol_max.x, 0.5f)) bounds_changed = true;
        if (bounds_changed) scene_sdf_pass().set_bounds(vol_min, vol_max);

        ImGui::Text("Resolution: %u", scene_sdf_pass().resolution());

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

        scene_sdf_pass().render_slice(cmd, static_cast<uint32_t>(sdf_debug_axis_), sdf_debug_slice_);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(gpu_.graphics_queue());
        vkFreeCommandBuffers(gpu_.device(), gpu_.command_pool(), 1, &cmd);

        // Display slice image
        auto imgui_set = scene_sdf_pass().slice_imgui_set();
        if (imgui_set) {
            float avail = ImGui::GetContentRegionAvail().x;
            float size = std::min(avail, 256.0f);
            ImGui::Image(imgui_set, {size, size});
        }
    }
    ImGui::End();
}

void VulkanContext::apply_pending_ddgi_toggle() {
    if (!has_pending_ddgi_toggle_) return;
    has_pending_ddgi_toggle_ = false;

    auto& config = const_cast<PostProcessConfig&>(default_pp_->config());
    if (config.ddgi == pending_ddgi_toggle_) return;

    // Wait for ALL GPU work to complete before touching shared resources
    submit_and_present(); // flush any pending recorded frame
    vkDeviceWaitIdle(gpu_.device());

    config.ddgi = pending_ddgi_toggle_;

    // Get current viewport size
    uint32_t w = 256, h = 256;
    auto* tm = default_pp_->tonemap();
    if (tm && tm->extent.width > 0) {
        w = tm->extent.width;
        h = tm->extent.height;
    }

    // Resize triggers create/destroy of DDGI images based on updated config
    default_pp_->resize(w, h);
}

void VulkanContext::reload_pipeline() {
    gpu_.pipeline()->recreate();
    gpu_.pipeline_cache()->recreate_all();
    particle_system_->recreate_pipelines();
    sync_custom_depth_pipelines();
}

void VulkanContext::sync_custom_depth_pipelines() {
    auto* pc = gpu_.pipeline_cache();
    if (!pc) return;

    for (uint32_t id = 0; id < pc->shader_count(); ++id) {
        if (!pc->has_custom_vertex(id)) continue;

        if (gpu_.mesh_shaders_available()) {
            auto* msd = passes_.try_get<MeshShaderDepthPass>("mesh_shader_depth");
            const auto& depth_mesh = pc->depth_mesh_spv(id);
            if (msd && !depth_mesh.empty()) {
                msd->register_custom_depth(id, depth_mesh);
            }
        }

        const auto& depth_vert = pc->depth_vert_spv(id);
        if (!depth_vert.empty()) {
            depth_prepass().register_custom_depth(id, depth_vert);
        }

        const auto& shadow_vert = pc->shadow_vert_spv(id);
        if (!shadow_vert.empty()) {
            shadow_pass().register_custom_shadow(id, shadow_vert);
        }
    }
}

// ── Scene Viewports ──────────────────────────────────────────────────────

uint32_t VulkanContext::create_scene_viewport(const SceneViewportConfig& config) {
    uint32_t bindless_idx = gpu_.bindless_textures().reserve_index();

    PostProcessResources::PassRefs refs{
        hiz_pass(), gtao_pass(), bloom_pass(),
        ssr_pass(), volumetric_fog_pass(), tonemap_pass(), ddgi_pass()};

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
