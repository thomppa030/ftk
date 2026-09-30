#include "renderer/systems/particle_system.hpp"
#include "core/small_vector.hpp"
#include "environment/environment_state.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/frustum.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/vertex.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_expression_eval.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/project.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>

namespace fjell {

// Must match the GPU Particle struct in particle_common.glsl
constexpr uint64_t PARTICLE_STRIDE = 80;

// Hard per-emitter cap on module_buffer_ usage. Curve modules run up to
// 528 bytes each; a handful of those plus the usual spawn/update stack can
// exceed a smaller budget, so this needs real headroom, not just the
// common case. write_gpu_modules() enforces this exactly — see its
// definition for why the enforcement has to live there.
constexpr uint32_t MODULE_BUDGET_BYTES = 2048;

// Sub-emitter events a parent may report per frame; matches the GLSL define.
constexpr uint32_t MAX_SUB_EVENTS_PER_EMITTER = 256;
// Bytes of one sub-emitter event: sizeof(SubEmitterEvent) in GLSL.
constexpr uint64_t SUB_EVENT_SIZE = 64;

namespace {

// Whether an emitter draws into the view being recorded: seen by the last
// simulation, and of the scene this view renders. Every scene's emitters live
// in one list, and a thumbnail or a camera preview draws without simulating,
// so the visibility flags alone would put the open scene's effects into a
// render of another.
bool draws_in(const ParticleSystem::EmitterGpu& em, const FrameContext& ctx) {
    return em.active && em.visible && ctx.scene != nullptr && em.scene_id == ctx.scene->id();
}

// Colour over what is there by the source's alpha, the alpha summed.
constexpr gpu::Blend ALPHA_BLEND{true, gpu::BlendFactor::src_alpha, gpu::BlendFactor::one_minus_src_alpha,
                                 gpu::BlendFactor::one, gpu::BlendFactor::one};
// Colour added by the source's alpha, the alpha summed.
constexpr gpu::Blend ADDITIVE_BLEND{true, gpu::BlendFactor::src_alpha, gpu::BlendFactor::one,
                                    gpu::BlendFactor::one, gpu::BlendFactor::one};
// Screen offsets summed.
constexpr gpu::Blend SUM_BLEND{true, gpu::BlendFactor::one, gpu::BlendFactor::one, gpu::BlendFactor::one,
                               gpu::BlendFactor::one};

// What the compute steps read and write, and what each step of a
// dispatch's sequence leaves for the next.
constexpr gpu::Access COMPUTE = gpu::Access::storage_buffer_read_write_compute;

} // namespace

void ParticleSystem::create(Device& /*device*/, const PassResources& resources) {
    gpu_device_ = resources.device;
    targets_ = resources;
    gpu::Device& gpu = *gpu_device_;

    // Soft particles read the scene's depth, which means nothing between two
    // texels.
    depth_sampler_ = gpu.sampler({.filter = gpu::Filter::nearest,
                                  .mip_filter = gpu::Filter::nearest,
                                  .address = gpu::Address::clamp});

    // Each buffer is made small and reserves room for the scene budgets in
    // particle_budget.hpp, which it grows into in place: growing waits for
    // nothing, and what is bound to it is never rewritten.
    constexpr uint64_t INITIAL_PARTICLES = 512;
    constexpr uint64_t INITIAL_EMITTERS = 8;
    auto make = [&](uint64_t per_item, uint64_t initial, uint64_t budget, gpu::BufferUses use,
                    std::string_view name) {
        auto made = gpu.create(gpu::BufferDesc{
            .size = initial * per_item, .reserve = budget * per_item, .use = use, .name = name});
        if (!made) throw std::runtime_error("Particles: " + made.error());
        return std::move(*made);
    };
    const gpu::BufferUses storage = gpu::BufferUse::storage;
    const gpu::BufferUses drawn = gpu::BufferUse::storage | gpu::BufferUse::indirect;
    particles_ = make(PARTICLE_STRIDE, INITIAL_PARTICLES, MAX_SCENE_PARTICLES, storage, "particle_mega");
    alive_ = make(sizeof(uint32_t), INITIAL_PARTICLES, MAX_SCENE_PARTICLES, storage, "particle_alive");
    mesh_instances_ = make(sizeof(GpuMeshInstance), INITIAL_PARTICLES, MAX_SCENE_PARTICLES, storage,
                           "particle_mesh_instances");
    counters_ = make(3 * sizeof(uint32_t), INITIAL_EMITTERS, MAX_SCENE_EMITTERS, storage, "particle_counters");
    indirect_ = make(sizeof(gpu::DrawArgs), INITIAL_EMITTERS, MAX_SCENE_EMITTERS, drawn, "particle_indirect");
    mesh_indirect_ = make(sizeof(gpu::DrawIndexedArgs), INITIAL_EMITTERS, MAX_SCENE_EMITTERS, drawn,
                          "particle_mesh_indirect");
    mesh_instance_counts_ = make(sizeof(uint32_t), INITIAL_EMITTERS, MAX_SCENE_EMITTERS, storage,
                                 "particle_mesh_counts");
    sub_events_ = make(MAX_SUB_EVENTS_PER_EMITTER * SUB_EVENT_SIZE, INITIAL_EMITTERS, MAX_SCENE_EMITTERS,
                       storage, "particle_sub_events");
    sub_event_counts_ = make(sizeof(uint32_t), INITIAL_EMITTERS, MAX_SCENE_EMITTERS, storage,
                             "particle_sub_event_counts");
    total_particle_capacity_ = INITIAL_PARTICLES;
    emitter_capacity_ = INITIAL_EMITTERS;

    if (!build_pipelines(pipelines_)) {
        throw std::runtime_error("Failed to build particle pipelines");
    }
    FJELL_GFX_DEBUG("Particle system created");
}

void ParticleSystem::recreate_pipelines() {
    Pipelines fresh;
    if (!build_pipelines(fresh)) {
        FJELL_GFX_ERROR("Particle pipelines not reloaded; the previous ones stay in use");
        return;
    }
    // Frames still in flight keep drawing with the pipelines replaced until
    // they are done.
    pipelines_ = std::move(fresh);
    FJELL_GFX_DEBUG("Particle pipelines reloaded");
}

bool ParticleSystem::build_pipelines(Pipelines& out) {
    gpu::Device& gpu = *gpu_device_;
    bool ok = true;

    // Only the simulation reads the globals: the scene's SDF, to collide.
    auto compute = [&](const char* shader, bool reads_globals) {
        auto made = gpu.create(gpu::ComputePipelineDesc{
            .shader = shader,
            .shared = reads_globals ? std::vector<gpu::SharedLayout>{targets_.globals}
                                    : std::vector<gpu::SharedLayout>{},
            .name = shader,
        });
        if (!made) {
            FJELL_GFX_ERROR("Particles: {}", made.error());
            ok = false;
            return gpu::Owned<gpu::ComputePipeline>{};
        }
        return std::move(*made);
    };
    out.emit = compute("shaders/particle_emit.comp", false);
    out.sim = compute("shaders/particle_sim.comp", true);
    out.indirect = compute("shaders/particle_indirect.comp", false);
    out.compact = compute("shaders/particle_compact.comp", false);

    // How a pipeline writes the scene targets. Blended ones leave
    // metallic/roughness and normal alone: those attachments have no alpha,
    // so a write would stamp the particle over the surface under the whole
    // quad, fringes included. Opaque ones write both. Nothing writes motion.
    enum class Look { additive, alpha, opaque };
    constexpr const char* BILLBOARD = "shaders/particle.vert";
    constexpr const char* TRAIL = "shaders/particle_trail.vert";
    constexpr const char* MESH = "shaders/particle_mesh.vert";
    constexpr const char* UNLIT = "shaders/particle.frag";
    constexpr const char* LIT = "shaders/particle_lit.frag";
    constexpr const char* OPAQUE = "shaders/particle_opaque.frag";
    constexpr const char* OPAQUE_LIT = "shaders/particle_opaque_lit.frag";
    constexpr auto LIST = gpu::Topology::triangles;
    constexpr auto STRIP = gpu::Topology::triangle_strip;
    auto draw = [&](const char* vertex, const char* fragment, gpu::Topology topology, Look look,
                    std::string_view name) {
        const bool opaque = look == Look::opaque;
        const gpu::Blend colour = look == Look::additive ? ADDITIVE_BLEND
                                  : look == Look::alpha  ? ALPHA_BLEND
                                                         : gpu::Blend::none;
        const gpu::ChannelMask surface =
            opaque ? gpu::ChannelMask{gpu::Channel::r | gpu::Channel::g} : gpu::ChannelMask{};
        // Mesh particles draw the mega buffer's vertices.
        const bool mesh = vertex == MESH;
        auto made = gpu.create(gpu::GraphicsPipelineDesc{
            .vertex = vertex,
            .fragment = fragment,
            .vertex_layout = mesh ? Vertex::layout() : gpu::VertexLayout{},
            .topology = topology,
            .depth = {.test = true, .write = opaque, .compare = gpu::Compare::less_equal},
            .color = {{targets_.scene_color_format, colour},
                      {targets_.scene_mr_format, gpu::Blend::none, surface},
                      {targets_.scene_normal_format, gpu::Blend::none, surface},
                      {targets_.scene_velocity_format, gpu::Blend::none, gpu::ChannelMask{}}},
            .depth_format = targets_.scene_depth_format,
            .samples = targets_.samples,
            .shared = {targets_.globals, targets_.bindless},
            .name = name,
        });
        if (!made) {
            FJELL_GFX_ERROR("Particles: {}", made.error());
            ok = false;
            return gpu::Owned<gpu::GraphicsPipeline>{};
        }
        return std::move(*made);
    };
    out.additive = draw(BILLBOARD, UNLIT, LIST, Look::additive, "particle_additive");
    out.alpha = draw(BILLBOARD, UNLIT, LIST, Look::alpha, "particle_alpha");
    out.trail_additive = draw(TRAIL, UNLIT, STRIP, Look::additive, "particle_trail_additive");
    out.trail_alpha = draw(TRAIL, UNLIT, STRIP, Look::alpha, "particle_trail_alpha");
    out.lit_additive = draw(BILLBOARD, LIT, LIST, Look::additive, "particle_lit_additive");
    out.lit_alpha = draw(BILLBOARD, LIT, LIST, Look::alpha, "particle_lit_alpha");
    out.lit_trail_additive = draw(TRAIL, LIT, STRIP, Look::additive, "particle_lit_trail_additive");
    out.lit_trail_alpha = draw(TRAIL, LIT, STRIP, Look::alpha, "particle_lit_trail_alpha");
    out.mesh_additive = draw(MESH, UNLIT, LIST, Look::additive, "particle_mesh_additive");
    out.mesh_alpha = draw(MESH, UNLIT, LIST, Look::alpha, "particle_mesh_alpha");
    out.mesh_lit_additive = draw(MESH, LIT, LIST, Look::additive, "particle_mesh_lit_additive");
    out.mesh_lit_alpha = draw(MESH, LIT, LIST, Look::alpha, "particle_mesh_lit_alpha");
    out.opaque = draw(BILLBOARD, OPAQUE, LIST, Look::opaque, "particle_opaque");
    out.opaque_lit = draw(BILLBOARD, OPAQUE_LIT, LIST, Look::opaque, "particle_opaque_lit");
    out.mesh_opaque = draw(MESH, OPAQUE, LIST, Look::opaque, "particle_mesh_opaque");
    out.mesh_opaque_lit = draw(MESH, OPAQUE_LIT, LIST, Look::opaque, "particle_mesh_opaque_lit");

    // Distortion: one R16G16 target of screen offsets, summed, single-sampled.
    auto distortion = gpu.create(gpu::GraphicsPipelineDesc{
        .vertex = BILLBOARD,
        .fragment = "shaders/particle_distortion.frag",
        .depth = {.test = true, .write = false, .compare = gpu::Compare::less_equal},
        .color = {{targets_.scene_distortion_format, SUM_BLEND, gpu::Channel::r | gpu::Channel::g}},
        .depth_format = targets_.scene_depth_format,
        .shared = {targets_.globals, targets_.bindless},
        .name = "particle_distortion",
    });
    if (!distortion) {
        FJELL_GFX_ERROR("Particles: {}", distortion.error());
        ok = false;
    } else {
        out.distortion = std::move(*distortion);
    }
    return ok;
}

void ParticleSystem::destroy() {
    pipelines_ = {};
    for (auto* buffer : {&particles_, &counters_, &indirect_, &alive_, &sub_events_, &sub_event_counts_,
                         &mesh_instances_, &mesh_instance_counts_, &mesh_indirect_}) {
        buffer->reset();
    }
    emitters_.clear();
}

void ParticleSystem::grow_buffers(uint32_t total_particles, uint32_t emitter_count) {
    auto grow = [&](gpu::Buffer buffer, uint64_t size) {
        if (auto grown = gpu_device_->grow(buffer, size); !grown) {
            FJELL_GFX_ERROR("Particles: {}", grown.error());
        }
    };
    const uint64_t particles = total_particles;
    const uint64_t emitters = emitter_count;
    grow(particles_, particles * PARTICLE_STRIDE);
    grow(alive_, particles * sizeof(uint32_t));
    grow(mesh_instances_, particles * sizeof(GpuMeshInstance));
    grow(counters_, emitters * 3 * sizeof(uint32_t));
    grow(indirect_, emitters * sizeof(gpu::DrawArgs));
    grow(mesh_indirect_, emitters * sizeof(gpu::DrawIndexedArgs));
    grow(mesh_instance_counts_, emitters * sizeof(uint32_t));
    grow(sub_events_, emitters * MAX_SUB_EVENTS_PER_EMITTER * SUB_EVENT_SIZE);
    grow(sub_event_counts_, emitters * sizeof(uint32_t));
    total_particle_capacity_ = total_particles;
    emitter_capacity_ = emitter_count;
    FJELL_GFX_DEBUG("Particle buffers grown: {} particles, {} emitters", total_particles, emitter_count);
}

void ParticleSystem::init_emitter_slots(const std::vector<uint32_t>& slots) {
    for (uint32_t i : slots) {
        const auto& em = emitters_[i];
        pending_inits_.push_back({i, em.base_offset, em.max_particles, true});
    }
}

void ParticleSystem::record_pending_inits(gpu::CommandList& cmd) {
    if (pending_inits_.empty()) return;

    // The dispatches of this pass and earlier frames read and write these
    // ranges; the fills come after them, and what follows sees the fills.
    cmd.barrier(particles_, COMPUTE, gpu::Access::clear);
    cmd.barrier(indirect_, COMPUTE, gpu::Access::clear);
    cmd.barrier(counters_, COMPUTE, gpu::Access::copy_dst);
    for (const auto& init : pending_inits_) {
        // A zeroed particle has lifetime 0: dead until emission overwrites it.
        const uint64_t particle_size = init.max_particles * PARTICLE_STRIDE;
        if (particle_size > 0) {
            cmd.fill(gpu::BufferRange(particles_, init.base_offset * PARTICLE_STRIDE, particle_size), 0);
        }
        if (!init.reset_counters) continue;

        cmd.fill(gpu::BufferRange(indirect_, init.slot * sizeof(gpu::DrawArgs), sizeof(gpu::DrawArgs)), 0);
        // alive, dead, trail_head
        const std::array<uint32_t, 3> counters{0, init.max_particles, 0};
        cmd.copy(cmd.transient(counters),
                 gpu::BufferRange(counters_, init.slot * sizeof(counters), sizeof(counters)));
    }
    pending_inits_.clear();
    cmd.barrier(particles_, gpu::Access::clear, COMPUTE);
    cmd.barrier(indirect_, gpu::Access::clear, COMPUTE);
    cmd.barrier(counters_, gpu::Access::copy_dst, COMPUTE);
}

// ── RenderSubsystem overrides ──────────────────────────────────────────

void ParticleSystem::sync(const RenderSubsystemContext& ctx) {
#ifndef NDEBUG
    (void)vfx_watcher_.poll();
    if (vfx_changed_ && ctx.scene && ctx.project) {
        reload_changed_vfx(ctx);
    }
    setup_vfx_watcher(ctx);
#endif
    if (ctx.scene) sync_scene(*ctx.scene, ctx.resource_manager, ctx.project);
}

void ParticleSystem::sync_scene(Scene& scene, ResourceManager* resources, const Project* project) {
    sync_emitters(scene);

    // Lazy-resolve mesh particle materials that weren't resolved on load
    if (resources) {
        for (auto [obj, ec] : scene.view<EmitterComponent>()) {
            if (!ec->effect) continue;
            for (auto& sub : ec->effect->emitters) {
                for (auto& rm : sub.render_modules) {
                    if (rm->type() != ModuleType::mesh_particle) continue;
                    auto* mm = static_cast<MeshParticleModule*>(rm.get());
                    if (mm->material_path.empty() || mm->resolved_material_id != 0) continue;
                    auto resolved = base_path::resolve_asset(mm->material_path, project);
                    auto result = resources->load_material_asset(resolved, project);
                    if (result) {
                        auto* mat = resources->get(*result);
                        if (mat) mm->resolved_material_id = mat->material_id();
                    }
                }
            }
        }
    }
}

void ParticleSystem::update(const RenderSubsystemContext& ctx) {
    // Cache view_projection for record_compute (GPU dispatch needs it for frustum).
    cached_view_projection_ = ctx.view_projection;
}

void ParticleSystem::record_compute(const FrameContext& ctx) {
    if (!ctx.scene) return;
    auto& scene_ref = const_cast<Scene&>(*ctx.scene);
    const glm::vec3 wind = ctx.environment != nullptr
        ? ctx.environment->weather.wind_direction *
              (ctx.environment->weather.wind_speed * ctx.environment->weather.wind_gust)
        : glm::vec3{0.0f};
    dispatch_compute(ctx, scene_ref, wind, cached_view_projection_);
}

void ParticleSystem::collect_exports(DeclareContext& ctx) const {
    auto add = [&](gpu::Buffer buffer, bool persistent, gpu::Access draw_access) {
        if (!buffer.valid()) { return; }
        ctx.subsystem_draw_inputs.push_back({BufferImport{buffer, persistent}, draw_access});
    };
    // The particles and their counters carry into the next frame; the alive
    // lists, commands and mesh instances are rebuilt from them every frame.
    add(particles_, true, gpu::Access::storage_buffer_read_vertex);
    add(counters_, true, gpu::Access::storage_buffer_read_vertex);
    add(alive_, false, gpu::Access::storage_buffer_read_vertex);
    add(indirect_, false, gpu::Access::indirect_read);
    add(mesh_instances_, false, gpu::Access::storage_buffer_read_vertex);
    add(mesh_indirect_, false, gpu::Access::indirect_read);
    // The ray-traced GI builds its particle instances from these.
    if (particles_) {
        ctx.buffer_imports.emplace(fg_name_hash("particle_state"), BufferImport{particles_, true});
    }
}

void ParticleSystem::record_opaque(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    record_render_opaque(ctx, pass);
}

void ParticleSystem::record_distortion(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    record_render_distortion(ctx, pass);
}

void ParticleSystem::record_transparent(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    record_render(ctx, pass);
}

void ParticleSystem::use(const FrameContext& ctx, gpu::RenderEncoder& pass, gpu::GraphicsPipeline pipeline) {
    pass.set_pipeline(pipeline);
    pass.bind(ctx.globals);
    pass.bind(ctx.bindless);
    // Each pairing of shaders reads its own few of these.
    const gpu::BindEntry offered[] = {
        {"ParticleBuffer", gpu::storage(particles_)},
        {"AliveList", gpu::storage(alive_)},
        {"EmitterBuffer", gpu::storage(emitter_records_)},
        {"Counters", gpu::storage(counters_)},
        {"InstanceBuffer", gpu::storage(mesh_instances_)},
        {"depth_tex", gpu::sampled(ctx.target->depth_texture(), depth_sampler_)},
    };
    const gpu::ShaderLayout& layout = gpu_device_->layout(pipeline);
    SmallVector<gpu::BindEntry, std::size(offered)> read;
    for (const gpu::BindEntry& entry : offered) {
        if (layout.find(entry.name) != nullptr) read.push_back(entry);
    }
    pass.bind(std::span<const gpu::BindEntry>(read.data(), read.size()));
}

void ParticleSystem::record_render_distortion(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    if (emitters_.empty() || !emitter_records_.buffer.valid()) return;

    bool has_any = false;
    for (const auto& em : emitters_) {
        if (draws_in(em, ctx) && em.is_distortion) { has_any = true; break; }
    }
    if (!has_any) return;
    auto zone = ctx.commands().zone("Particles: Distortion");

    use(ctx, pass, pipelines_.distortion);
    for (uint32_t i = 0; i < static_cast<uint32_t>(emitters_.size()); ++i) {
        if (!draws_in(emitters_[i], ctx) || !emitters_[i].is_distortion) continue;
        pass.push(emitters_[i].distortion_strength);
        pass.draw_indirect(gpu::BufferRange(indirect_, i * sizeof(gpu::DrawArgs), sizeof(gpu::DrawArgs)), 1,
                           sizeof(gpu::DrawArgs));
    }
}

// ── Internal implementations ───────────────────────────────────────────

void ParticleSystem::release_scene(uint64_t scene_id) {
    for (auto& gpu : emitters_) {
        if (gpu.active && gpu.scene_id == scene_id) free_slot(gpu);
    }
}

uint32_t ParticleSystem::allocate_slot() {
    for (uint32_t i = 0; i < static_cast<uint32_t>(emitters_.size()); ++i) {
        if (!emitters_[i].active) return i;
    }
    emitters_.emplace_back();
    return static_cast<uint32_t>(emitters_.size() - 1);
}

std::optional<uint32_t> ParticleSystem::allocate_particle_range(uint32_t count) const {
    std::vector<ParticleRange> used;
    used.reserve(emitters_.size());
    for (const auto& gpu : emitters_) {
        if (gpu.active) used.emplace_back(gpu.base_offset, gpu.base_offset + gpu.max_particles);
    }
    return first_fit_range(std::move(used), count, MAX_SCENE_PARTICLES);
}

void ParticleSystem::free_slot(EmitterGpu& gpu) {
    // Its particles are killed at the next dispatch rather than left to
    // expire: a trail's never would, and until then they would be simulated
    // against whatever the slot holds next. Everything a consumer might
    // test is cleared so the slot reads as empty to the draw and DDGI
    // loops that walk it.
    pending_inits_.push_back({0, gpu.base_offset, gpu.max_particles, false});
    gpu = EmitterGpu{};
}

uint32_t ParticleSystem::particle_extent() const {
    uint32_t extent = 0;
    for (const auto& gpu : emitters_) {
        if (gpu.active) extent = std::max(extent, gpu.base_offset + gpu.max_particles);
    }
    return extent;
}

void ParticleSystem::sync_emitters(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("particle_sync_emitters");

    // Collect current scene emitters
    struct EmitterKey {
        uint32_t object_id;
        uint32_t emitter_index;
        uint32_t max_particles;
        ParticleBlendMode blend_mode;
    };
    const uint64_t scene_id = scene.id();
    std::vector<EmitterKey> scene_emitters;
    for (auto [obj, ec] : scene.view<EmitterComponent>()) {
        if (!ec->effect) continue;
        for (uint32_t i = 0; i < static_cast<uint32_t>(ec->effect->emitters.size()); ++i) {
            auto& sub = ec->effect->emitters[i];
            scene_emitters.push_back({obj->id, i, sub.max_particles, sub.blend_mode});
        }
    }

    auto key_match = [scene_id](const EmitterGpu& gpu, const EmitterKey& key) {
        return gpu.active && gpu.scene_id == scene_id && gpu.object_id == key.object_id
            && gpu.emitter_index == key.emitter_index;
    };

    // Free this scene's dead emitters. Other scenes' emitters are not
    // this sync's to judge; they stay until their own scene is synced or
    // released.
    for (auto& gpu : emitters_) {
        if (!gpu.active || gpu.scene_id != scene_id) continue;
        bool alive = false;
        for (const auto& se : scene_emitters) {
            if (key_match(gpu, se)) { alive = true; break; }
        }
        if (!alive) free_slot(gpu);
    }

    // Find new emitters not in the current list
    std::vector<EmitterKey> new_emitters;
    for (const auto& se : scene_emitters) {
        bool found = false;
        for (const auto& existing : emitters_) {
            if (key_match(existing, se)) { found = true; break; }
        }
        if (!found) new_emitters.push_back(se);
    }

    if (new_emitters.empty()) return;

    FJELL_GFX_DEBUG("Particle sync: {} slots, {} new",
                    emitters_.size(), new_emitters.size());

    std::vector<uint32_t> new_slots;
    for (const auto& ne : new_emitters) {
        // Every effect in the scene shares one particle budget. An emitter
        // that would take the scene past it is refused here, naming itself
        // and what it asked for, rather than growing the buffer until it
        // throws with nothing to act on: a greedy effect is an authoring
        // mistake and the scene should keep running while it is fixed.
        auto range = allocate_particle_range(ne.max_particles);
        if (!range) {
            uint32_t held = 0;
            for (const auto& gpu : emitters_) {
                if (gpu.active) held += gpu.max_particles;
            }
            FJELL_GFX_WARN("Particles: emitter {} of node {} asked for {} particles, "
                           "but only {} of the scene's {} are left — it will not emit. "
                           "Lower its Max Particles, or another effect's.",
                           ne.emitter_index, ne.object_id, ne.max_particles,
                           MAX_SCENE_PARTICLES - held, MAX_SCENE_PARTICLES);
            continue;
        }
        uint32_t slot = allocate_slot();
        auto& gpu = emitters_[slot];
        gpu.active = true;
        gpu.scene_id = scene_id;
        gpu.object_id = ne.object_id;
        gpu.emitter_index = ne.emitter_index;
        gpu.max_particles = ne.max_particles;
        gpu.base_offset = *range;
        gpu.blend_mode = ne.blend_mode;
        new_slots.push_back(slot);
    }

    uint32_t total = particle_extent();
    uint32_t count = static_cast<uint32_t>(emitters_.size());

    // The buffers grow in place, keeping their handles, so nothing waits
    // for the device.
    if (total > total_particle_capacity_ || count > emitter_capacity_) {
        uint32_t new_total = grow_buffer_to(total, total_particle_capacity_, MAX_SCENE_PARTICLES);
        uint32_t new_count = grow_buffer_to(count, emitter_capacity_, MAX_SCENE_EMITTERS);
        grow_buffers(new_total, new_count);
    }
    init_emitter_slots(new_slots);
}

void ParticleSystem::upload_primitives(MegaBuffer& mega_buffer) {
    if (primitives_uploaded_) return;

    // ── Cube (24 vertices, 36 indices) ──
    std::vector<Vertex> cube_verts;
    std::vector<uint32_t> cube_idx;
    {
        struct Face { glm::vec3 normal; glm::vec3 up; glm::vec3 right; };
        Face faces[] = {
            {{ 0, 0, 1}, {0,1,0}, {1,0,0}},  // +Z
            {{ 0, 0,-1}, {0,1,0}, {-1,0,0}}, // -Z
            {{ 1, 0, 0}, {0,1,0}, {0,0,-1}}, // +X
            {{-1, 0, 0}, {0,1,0}, {0,0,1}},  // -X
            {{ 0, 1, 0}, {0,0,1}, {1,0,0}},  // +Y
            {{ 0,-1, 0}, {0,0,-1},{1,0,0}},  // -Y
        };
        for (const auto& f : faces) {
            uint32_t base = static_cast<uint32_t>(cube_verts.size());
            glm::vec3 corners[4] = {
                f.normal * 0.5f - f.right * 0.5f - f.up * 0.5f,
                f.normal * 0.5f + f.right * 0.5f - f.up * 0.5f,
                f.normal * 0.5f + f.right * 0.5f + f.up * 0.5f,
                f.normal * 0.5f - f.right * 0.5f + f.up * 0.5f,
            };
            glm::vec2 uvs[4] = {{0,0},{1,0},{1,1},{0,1}};
            glm::vec4 tan = glm::vec4(f.right, 1.0f);
            for (int i = 0; i < 4; ++i) {
                cube_verts.push_back({corners[i], {1,1,1}, uvs[i], f.normal, tan});
            }
            cube_idx.insert(cube_idx.end(), {base, base+1, base+2, base, base+2, base+3});
        }
    }
    mesh_cube_ = mega_buffer.upload(cube_verts, cube_idx);

    // ── Sphere (UV sphere, 16 segments × 12 rings) ──
    std::vector<Vertex> sphere_verts;
    std::vector<uint32_t> sphere_idx;
    {
        constexpr int segments = 16;
        constexpr int rings = 12;
        for (int r = 0; r <= rings; ++r) {
            float phi = glm::pi<float>() * static_cast<float>(r) / static_cast<float>(rings);
            float sp = std::sin(phi);
            float cp = std::cos(phi);
            for (int s = 0; s <= segments; ++s) {
                float theta = 2.0f * glm::pi<float>() * static_cast<float>(s) / static_cast<float>(segments);
                float st = std::sin(theta);
                float ct = std::cos(theta);
                glm::vec3 pos{sp * ct * 0.5f, cp * 0.5f, sp * st * 0.5f};
                glm::vec3 normal = glm::normalize(pos);
                glm::vec2 uv{static_cast<float>(s) / segments, static_cast<float>(r) / rings};
                glm::vec3 tangent_dir = glm::normalize(glm::vec3(-st, 0.0f, ct));
                sphere_verts.push_back({pos, {1,1,1}, uv, normal, glm::vec4(tangent_dir, 1.0f)});
            }
        }
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < segments; ++s) {
                uint32_t a = static_cast<uint32_t>(r * (segments + 1) + s);
                uint32_t b = a + static_cast<uint32_t>(segments) + 1;
                sphere_idx.insert(sphere_idx.end(), {a, b, a + 1, b, b + 1, a + 1});
            }
        }
    }
    mesh_sphere_ = mega_buffer.upload(sphere_verts, sphere_idx);

    primitives_uploaded_ = true;
}

static glm::vec3 compute_spawn_extent(const VfxEmitter& emitter) {
    glm::vec3 extent{0.0f};
    for (const auto& mod : emitter.spawn_modules) {
        if (!mod->enabled) continue;
        if (auto* pos = dynamic_cast<const InitialPositionModule*>(mod.get())) {
            switch (pos->shape) {
                case SpawnShape::sphere: extent = glm::vec3(pos->radius); break;
                case SpawnShape::box:    extent = pos->half_extents; break;
                case SpawnShape::cone:   extent = glm::vec3(pos->radius, 0.0f, pos->radius); break;
                case SpawnShape::ring:   extent = glm::vec3(pos->radius, 0.0f, pos->radius); break;
                case SpawnShape::disk:   extent = glm::vec3(pos->radius, 0.0f, pos->radius); break;
                case SpawnShape::edge:   extent = glm::vec3(pos->edge_length * 0.5f, 0.0f, 0.0f); break;
                default: break;
            }
        }
    }
    return extent;
}

static float max_velocity_magnitude(const VfxEmitter& emitter) {
    float max_speed = 0.0f;
    for (const auto& mod : emitter.spawn_modules) {
        if (!mod->enabled) continue;
        if (auto* vel = dynamic_cast<const InitialVelocityModule*>(mod.get())) {
            max_speed = std::max(max_speed, glm::length(vel->velocity_max));
            max_speed = std::max(max_speed, vel->speed_max);
        }
    }
    return max_speed;
}

void ParticleSystem::dispatch_compute(const FrameContext& ctx, Scene& scene, const glm::vec3& wind,
                                      const glm::mat4& view_projection) {
    FJELL_PROFILE_SCOPE_N("particle_dispatch_compute");
    if (emitters_.empty()) return;
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Particles: Compute");
    const glm::vec3 camera_pos = ctx.camera_position;
    float dt = ctx.delta_time;
    record_pending_inits(cmd);
    // One long frame would otherwise move every particle far enough in a
    // single step to pass through whatever it was about to hit. Past this
    // the simulation runs slow instead.
    constexpr float MAX_STEP = 1.0f / 15.0f;
    dt = std::min(dt, MAX_STEP);
    ++frame_seed_;
    accumulated_time_ += dt;
    camera_pos_ = camera_pos;
    Frustum frustum = Frustum::from_vp_matrix_zo(view_projection);

    uint32_t emitter_count = static_cast<uint32_t>(emitters_.size());
    // Ranges are allocated first fit, so a freed gap below a live emitter
    // is part of what the simulation has to cover.
    uint32_t total_max = particle_extent();

    // Build emitter metadata
    emitter_data_cache_.clear();
    emitter_data_cache_.resize(emitter_count);
    uint32_t max_emit_count = 0;
    uint32_t max_child_emit = 0; // threads the child emit pass needs across emitters

    // Room for every emitter's whole budget; what is sent is what was written.
    module_bytes_.resize(static_cast<size_t>(emitter_count) * MODULE_BUDGET_BYTES);
    auto* module_ptr = reinterpret_cast<uint8_t*>(module_bytes_.data());
    uint32_t module_word_offset = 0;

    for (size_t i = 0; i < emitters_.size(); ++i) {
        auto& gpu = emitters_[i];
        // An emitter of another scene is looked up nowhere: a node id is
        // only unique within its scene, and it has to freeze, not follow a
        // stranger's node. A free slot has no node at all.
        gpu.in_scene = gpu.active && gpu.scene_id == scene.id();
        auto* obj = gpu.in_scene ? scene.find(gpu.object_id) : nullptr;
        auto& data = emitter_data_cache_[i];
        std::memset(&data, 0, sizeof(data));

        auto* ec = obj ? obj->components.get<EmitterComponent>() : nullptr;
        if (!ec || !ec->effect || !obj->as_spatial()
            || gpu.emitter_index >= ec->effect->emitters.size()) {
            // Emitter not valid — assign a terminator-only module block
            // so the shader exits the module walk immediately. With dt and
            // emit count zero its particles hold still, and unseen they
            // are not drawn into a target that is not theirs.
            gpu.visible = false;
            data.module_offset = module_word_offset;
            // Zero would name emitter 0 as its parent, and the sub-emit
            // pass would then spawn from that emitter's events into a
            // ring of zero particles.
            data.sub_event_source = 0xFFFFFFFF;
            auto* term = reinterpret_cast<uint32_t*>(module_ptr + module_word_offset * 4);
            term[0] = 0xFFFFFFFF; // MOD_TERMINATOR
            term[1] = 0;
            module_word_offset += 2;
            continue;
        }
        auto* spatial = obj->as_spatial();

        auto& sub = ec->effect->emitters[gpu.emitter_index];

        // Manual fire button — reset burst for all sub-emitters. One that
        // has played out its loops starts a fresh cycle, past its delay.
        if (ec->fire) {
            sub.burst_fired = false;
            sub.burst_elapsed = 0.0f;
            sub.local_elapsed = 0.0f;
            if (sub.finished) {
                sub.finished = false;
                sub.loops_completed = 0;
                sub.emitter_elapsed = sub.start_delay;
            }
        }

        // CPU-side emission accumulation (per sub-emitter). A burst is an
        // event: it plays once whether or not the camera sees it, so an
        // explosion behind the player is not lost. The rate is the ongoing
        // cost, and that is what culling and distance LOD scale.
        float eff_dt = sub.paused ? 0.0f : dt;
        uint32_t burst_emit = 0;
        uint32_t rate_emit = 0;

        // Advance emitter elapsed time
        if (!sub.paused && !sub.finished) {
            sub.emitter_elapsed += dt;
            sub.local_elapsed += dt;
        }

        // Timing gate: delay and duration
        bool in_delay = sub.emitter_elapsed < sub.start_delay;
        float eff_dur = sub.effective_duration();
        float time_since_start = sub.emitter_elapsed - sub.start_delay;
        bool past_duration = (eff_dur > 0.0f) && (time_since_start >= eff_dur);
        bool emitting = !sub.paused && !sub.finished && !in_delay && !past_duration;

        // Handle loop boundaries — restart immediately (ring buffer overwrites dead slots)
        if (past_duration && !sub.finished && !sub.paused) {
            sub.loops_completed++;
            bool should_loop = ec->looping
                && (sub.loop_count == 0 || sub.loops_completed < sub.loop_count);
            if (should_loop) {
                sub.emitter_elapsed = sub.start_delay;
                sub.burst_fired = false;
                sub.burst_elapsed = 0.0f;
                sub.local_elapsed = 0.0f;
                sub.emit_accumulator = 0.0f;
                emitting = true;
            } else {
                sub.finished = true;
            }
        }

        if (emitting) {
            uint32_t burst = sub.burst_count();
            if (burst > 0 && !sub.burst_fired) {
                burst_emit = burst;
                sub.burst_fired = true;
                sub.burst_elapsed = 0.0f;
            }

            // Looping burst within a single emitter cycle
            if (burst > 0 && sub.burst_fired && ec->looping) {
                sub.burst_elapsed += dt;
                if (sub.burst_elapsed >= sub.lifetime_max()) {
                    sub.burst_fired = false;
                    sub.burst_elapsed = 0.0f;
                }
            }

            sub.emit_accumulator += sub.emission_rate() * dt;
            rate_emit += static_cast<uint32_t>(sub.emit_accumulator);
            sub.emit_accumulator -= std::floor(sub.emit_accumulator);
        }

        glm::vec3 pos = glm::vec3(spatial->world_matrix_[3]);
        data.emitter_position = pos;
        if (gpu.has_prev_position) {
            data.emitter_velocity = (pos - gpu.prev_position) / std::max(dt, 0.0001f);
        } else {
            data.emitter_velocity = glm::vec3(0.0f);
        }
        // Distance-based emission (before updating prev_position)
        if (gpu.has_prev_position && !sub.paused) {
            float frame_dist = glm::length(pos - gpu.prev_position);
            for (const auto& mod : sub.spawn_modules) {
                if (mod->enabled && mod->type() == ModuleType::emit_over_distance) {
                    auto* eod = static_cast<const EmitOverDistanceModule*>(mod.get());
                    sub.distance_accumulator += frame_dist * eod->particles_per_meter;
                    rate_emit += static_cast<uint32_t>(sub.distance_accumulator);
                    sub.distance_accumulator -= std::floor(sub.distance_accumulator);
                    break;
                }
            }
        }

        gpu.prev_position = pos;
        gpu.has_prev_position = true;

        // ── Frustum culling + distance LOD ──
        glm::vec3 spawn_ext = compute_spawn_extent(sub);
        float max_travel = sub.lifetime_max() * max_velocity_magnitude(sub);
        AABB emitter_aabb;
        emitter_aabb.min = pos - spawn_ext - glm::vec3(max_travel);
        emitter_aabb.max = pos + spawn_ext + glm::vec3(max_travel);
        gpu.visible = frustum.intersects(emitter_aabb);

        if (gpu.visible) {
            float dist = glm::length(camera_pos - pos);
            float lod_factor = 1.0f;
            if (dist > 100.0f)      lod_factor = 0.0f;
            else if (dist > 60.0f)  lod_factor = 0.25f;
            else if (dist > 30.0f)  lod_factor = 0.5f;
            rate_emit = static_cast<uint32_t>(static_cast<float>(rate_emit) * lod_factor);
        } else {
            rate_emit = 0;
        }
        uint32_t emit_count = burst_emit + rate_emit;
        max_emit_count = std::max(max_emit_count, emit_count);

        data.dt = eff_dt;
        data.max_particles = gpu.max_particles;
        data.emit_count = emit_count;
        data.random_seed = frame_seed_ ^ ((gpu.object_id * 31 + gpu.emitter_index) * 2654435761u);
        data.base_offset = gpu.base_offset;
        data.counter_offset = static_cast<uint32_t>(i) * 3;

        // Detect trail emitter from render modules
        bool is_trail = false;
        float trail_width = 0.1f;
        float trail_wol = 1.0f;
        for (const auto& rm : sub.render_modules) {
            if (rm->enabled && rm->type() == ModuleType::trail) {
                is_trail = true;
                auto* tm = dynamic_cast<const TrailModule*>(rm.get());
                if (tm) { trail_width = tm->width; trail_wol = tm->width_over_life; }
                break;
            }
        }
        // Detect lit emitter from render modules
        bool is_lit = false;
        float lit_metallic = 0.0f;
        float lit_roughness = 0.5f;
        for (const auto& rm : sub.render_modules) {
            if (rm->enabled && rm->type() == ModuleType::lit_particle) {
                is_lit = true;
                auto* lm = dynamic_cast<const LitParticleModule*>(rm.get());
                if (lm) { lit_metallic = lm->metallic; lit_roughness = lm->roughness; }
                break;
            }
        }
        gpu.is_lit = is_lit;
        gpu.lit_metallic = lit_metallic;
        gpu.lit_roughness = lit_roughness;

        // Detect mesh particle emitter from render modules
        bool is_mesh = false;
        ParticleMeshShape mesh_shape = ParticleMeshShape::cube;
        uint32_t mesh_material_id = 0;
        for (const auto& rm : sub.render_modules) {
            if (rm->enabled && rm->type() == ModuleType::mesh_particle) {
                is_mesh = true;
                auto* mm = dynamic_cast<const MeshParticleModule*>(rm.get());
                if (mm) {
                    mesh_shape = mm->shape;
                    mesh_material_id = mm->resolved_material_id;
                }
                break;
            }
        }
        gpu.is_mesh = is_mesh;
        gpu.mesh_shape = mesh_shape;
        gpu.mesh_material_id = mesh_material_id;
        if (is_mesh) {
            switch (mesh_shape) {
                case ParticleMeshShape::cube:   gpu.mesh_entry = mesh_cube_; break;
                case ParticleMeshShape::sphere: gpu.mesh_entry = mesh_sphere_; break;
                default: gpu.mesh_entry = mesh_cube_; break;
            }
        }
        // Mirror the resolved mesh draw range onto the GPU emitter record so
        // the mesh-instance compute pass can build its indirect command
        // without any CPU round-trip.
        data.mesh_vertex_offset = gpu.mesh_entry.vertex_offset;
        data.mesh_first_index = gpu.mesh_entry.first_index;
        data.mesh_index_count = gpu.mesh_entry.index_count;

        // Detect distortion emitter from render modules.
        // Distortion is a standalone render mode (billboard quads → UV offset buffer).
        // Only active when the emitter has no trail/lit/mesh overrides.
        bool is_distortion = false;
        float distortion_strength = 0.05f;
        if (!is_trail && !is_lit && !is_mesh) {
            for (const auto& rm : sub.render_modules) {
                if (rm->enabled && rm->type() == ModuleType::distortion) {
                    is_distortion = true;
                    auto* dm = dynamic_cast<const DistortionModule*>(rm.get());
                    if (dm) distortion_strength = dm->strength;
                    break;
                }
            }
        }
        gpu.is_distortion = is_distortion;
        gpu.distortion_strength = distortion_strength;

        gpu.is_trail = is_trail;
        data.flags = (is_trail ? 1u : 0u) | (is_lit ? 2u : 0u) | (is_mesh ? 4u : 0u);
        for (const auto& mod : sub.update_modules) {
            if (mod->enabled && mod->type() == ModuleType::sub_emitter) {
                auto* sem = static_cast<const SubEmitterModule*>(mod.get());
                data.flags |= 8u | (static_cast<uint32_t>(sem->trigger) << 4);
                break;
            }
        }
        data.trail_width = trail_width;
        data.trail_width_over_life = trail_wol;

        // Sub-emitter fields: detect if this emitter is a child of another
        // sub_event_offset: where this emitter's events are stored (as parent)
        // sub_event_source: which emitter is the parent (0xFFFFFFFF if none)
        data.sub_event_offset = static_cast<uint32_t>(i) * MAX_SUB_EVENTS_PER_EMITTER;
        data.sub_event_source = 0xFFFFFFFF; // not a child by default
        data.sub_burst_count = 0;
        data.sub_vel_inherit = 0.0f;

        // Check if any parent emitter has a SubEmitterModule pointing to us.
        // We match by emitter_index within the same VfxEffect: the same
        // object in the same scene, since node ids repeat across scenes.
        for (uint32_t pi = 0; pi < emitter_count; ++pi) {
            if (pi == i) continue;
            if (!emitters_[pi].active) continue;
            if (emitters_[pi].scene_id != gpu.scene_id) continue;
            if (emitters_[pi].object_id != gpu.object_id) continue;
            // Find the parent's VfxEmitter to check for SubEmitterModule
            auto* parent_obj = scene.find(emitters_[pi].object_id);
            if (!parent_obj) continue;
            auto* parent_ec = parent_obj->components.get<EmitterComponent>();
            if (!parent_ec || !parent_ec->effect) continue;
            if (emitters_[pi].emitter_index >= parent_ec->effect->emitters.size()) continue;
            auto& parent_sub = parent_ec->effect->emitters[emitters_[pi].emitter_index];
            for (const auto& mod : parent_sub.update_modules) {
                if (mod->enabled && mod->type() == ModuleType::sub_emitter) {
                    auto* sem = dynamic_cast<const SubEmitterModule*>(mod.get());
                    if (sem && sem->child_emitter_index == gpu.emitter_index) {
                        data.sub_event_source = pi;
                        data.sub_burst_count = sem->burst_count;
                        data.sub_vel_inherit = sem->velocity_inherit;
                        break;
                    }
                }
            }
            if (data.sub_event_source != 0xFFFFFFFF) break;
        }

        // Child emitters never emit on their own — only from events. A
        // parent reports at most MAX_SUB_EVENTS_PER_EMITTER events a frame
        // and each spawns a burst.
        if (data.sub_event_source != 0xFFFFFFFF) {
            data.emit_count = 0;
            max_child_emit = std::max(max_child_emit,
                                      MAX_SUB_EVENTS_PER_EMITTER * std::max(data.sub_burst_count, 1u));
        }

        // Evaluate expression graph (writes computed values to module members).
        // Local time restarts at 0 on every burst fire / loop restart, so a
        // graph built on sin(time)/cos(time) etc. replays identically each
        // time instead of sampling whatever phase the engine's shared clock
        // happens to be at.
        float expr_time = sub.instanced_time ? sub.local_elapsed : accumulated_time_;
        evaluate_expression_graph(sub, expr_time, dt, wind);

        // Write module data. Each emitter gets a fixed MODULE_BUDGET_BYTES
        // window; write_gpu_modules() itself enforces that budget (dropping
        // any module that would overflow it) so a heavy module stack can
        // never bleed into the next emitter's window — see its definition
        // for why that check has to live there rather than out here.
        data.module_offset = module_word_offset;
        uint32_t module_buf_capacity = static_cast<uint32_t>(module_bytes_.size());
        uint32_t byte_offset = module_word_offset * 4;
        if (byte_offset + MODULE_BUDGET_BYTES > module_buf_capacity) {
            FJELL_GFX_ERROR("Module buffer overflow: offset {} + {} > capacity {}",
                            byte_offset, MODULE_BUDGET_BYTES, module_buf_capacity);
            // Still advance past this emitter's full window so the next
            // emitter doesn't alias it — only the module *contents* are
            // skipped, not its slot in the buffer's layout.
            module_word_offset += MODULE_BUDGET_BYTES / 4;
            continue;
        }
        // Inject current SDF volume bounds into collision modules
        for (auto& mod : sub.update_modules) {
            if (mod->type() == ModuleType::collision) {
                auto& col = static_cast<CollisionModule&>(*mod);
                col.sdf_volume_min = sdf_volume_min_;
                col.sdf_volume_max = sdf_volume_max_;
            }
        }

        uint32_t bytes = sub.write_gpu_modules(module_ptr + byte_offset, MODULE_BUDGET_BYTES);
        module_word_offset += bytes / 4;
    }

    // Clear one-frame fire flags
    for (size_t i = 0; i < emitters_.size(); ++i) {
        if (!emitters_[i].in_scene) continue;
        auto* obj = scene.find(emitters_[i].object_id);
        auto* ec = obj ? obj->components.get<EmitterComponent>() : nullptr;
        if (ec) ec->fire = false;
    }

    // This dispatch's emitter records and module bytes, which the frame's
    // trail draws read too.
    emitter_records_ = cmd.transient(std::span<const GpuEmitterData>(emitter_data_cache_.data(), emitter_count));
    const gpu::BufferRange modules = cmd.transient(
        std::span<const std::byte>(module_bytes_.data(), static_cast<size_t>(module_word_offset) * 4));

    ParticleComputePush push{};
    push.dt = dt;
    push.random_seed = frame_seed_;
    push.emitter_count = emitter_count;
    push.total_max_particles = total_max;

    // Each step sees what the one before it wrote to `written`.
    auto settle = [&](std::initializer_list<gpu::Buffer> written) {
        for (gpu::Buffer buffer : written) cmd.barrier(buffer, COMPUTE, COMPUTE);
    };
    const gpu::BindEntry simulation[] = {
        {"ParticleBuffer", gpu::storage(particles_)},
        {"Counters", gpu::storage(counters_)},
        {"EmitterBuffer", gpu::storage(emitter_records_)},
        {"ModuleBuffer", gpu::storage(modules)},
        {"SubEmitterEventBuf", gpu::storage(sub_events_)},
        {"SubEmitterEventCounters", gpu::storage(sub_event_counts_)},
    };
    auto use_simulation = [&](gpu::ComputePipeline pipeline) {
        cmd.set_pipeline(pipeline);
        cmd.bind(simulation);
        cmd.push(push);
    };

    // Sub-emitter events counted afresh. The last frame's steps are done with
    // the counts before the clear, and this frame's wait for it.
    const gpu::BufferRange event_counts(sub_event_counts_, 0, uint64_t{emitter_count} * sizeof(uint32_t));
    cmd.barrier(event_counts, COMPUTE, gpu::Access::clear);
    cmd.fill(event_counts, 0);
    cmd.barrier(event_counts, gpu::Access::clear, COMPUTE);

    // ── Phase 1: Emit normal emitters (sub_emit_pass=0, skip children) ──
    push.sub_emit_pass = 0;
    if (max_emit_count > 0) {
        auto step = cmd.zone("Particles: Emit");
        use_simulation(pipelines_.emit);
        cmd.dispatch((max_emit_count + 255) / 256, emitter_count, 1);
        settle({particles_, counters_});
    }

    // ── Phase 2: Simulate (writes sub-emitter events on death/collision) ──
    {
        auto step = cmd.zone("Particles: Simulate");
        use_simulation(pipelines_.sim);
        cmd.bind(ctx.globals);
        cmd.dispatch((total_max + 255) / 256, 1, 1);
        settle({particles_, counters_, sub_events_, sub_event_counts_});
    }

    // ── Phase 3: Emit child emitters (sub_emit_pass=1, consume events) ──
    push.sub_emit_pass = 1;
    if (max_child_emit > 0) {
        auto step = cmd.zone("Particles: Emit Children");
        use_simulation(pipelines_.emit);
        cmd.dispatch((max_child_emit + 255) / 256, emitter_count, 1);
        settle({particles_, counters_});
    }

    // ── Phase 4: Indirect prep (billboard/trail emitters) ──
    push.sub_emit_pass = 0;
    {
        auto step = cmd.zone("Particles: Indirect");
        cmd.set_pipeline(pipelines_.indirect);
        cmd.bind({{"Counters", gpu::storage(counters_)},
                  {"EmitterBuffer", gpu::storage(emitter_records_)},
                  {"IndirectDraw", gpu::storage(indirect_)}});
        cmd.push(push);
        cmd.dispatch((emitter_count + 255) / 256, 1, 1);
        settle({indirect_});
    }

    // ── Phase 5: Compaction (billboard and mesh emitters) ──
    // Gathers each emitter's alive particles into a dense range and writes
    // its indirect command from the count, so a draw costs vertex-shader
    // work for what is alive instead of the whole budget every frame.
    // Three sub-passes (reset/compact/finalize) because GLSL's barrier()
    // only orders invocations within one workgroup, and max_particles
    // routinely spans several (local_size_x = 256) — see
    // particle_compact.comp's header comment.
    uint32_t max_slots = 0;
    for (const auto& em : emitters_) {
        if (em.active && !em.is_trail) max_slots = std::max(max_slots, em.max_particles);
    }
    if (max_slots > 0) {
        auto step = cmd.zone("Particles: Mesh Compact");
        cmd.set_pipeline(pipelines_.compact);
        cmd.bind({{"ParticleBuffer", gpu::storage(particles_)},
                  {"MeshIndirect", gpu::storage(mesh_indirect_)},
                  {"BillboardIndirect", gpu::storage(indirect_)},
                  {"EmitterBuffer", gpu::storage(emitter_records_)},
                  {"AliveList", gpu::storage(alive_)},
                  {"InstanceBuffer", gpu::storage(mesh_instances_)},
                  {"AliveCounts", gpu::storage(mesh_instance_counts_)}});
        const auto compacted = {mesh_indirect_.get(), indirect_.get(), alive_.get(), mesh_instances_.get(),
                                mesh_instance_counts_.get()};

        push.sub_emit_pass = 0; // reset
        cmd.push(push);
        cmd.dispatch(1, emitter_count, 1);
        settle(compacted);

        push.sub_emit_pass = 1; // compact
        cmd.push(push);
        cmd.dispatch((max_slots + 255) / 256, emitter_count, 1);
        settle(compacted);

        push.sub_emit_pass = 2; // finalize
        cmd.push(push);
        cmd.dispatch(1, emitter_count, 1);
    }
}

void ParticleSystem::record_render(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    FJELL_PROFILE_SCOPE_N("particle_record_render");
    if (emitters_.empty() || !emitter_records_.buffer.valid()) return;
    auto zone = ctx.commands().zone("Particles: Transparent");

    // Build sorted draw order: additive first (order-independent),
    // then alpha-blended back-to-front by distance to camera.
    std::vector<uint32_t> draw_order;
    draw_order.reserve(emitters_.size());

    std::vector<uint32_t> alpha_indices;
    for (uint32_t i = 0; i < static_cast<uint32_t>(emitters_.size()); ++i) {
        if (!draws_in(emitters_[i], ctx)) continue;
        if (emitters_[i].blend_mode == ParticleBlendMode::opaque) continue;
        if (emitters_[i].is_distortion) continue; // drawn in distortion pass
        if (emitters_[i].blend_mode == ParticleBlendMode::additive) {
            draw_order.push_back(i);
        } else {
            alpha_indices.push_back(i);
        }
    }

    // Sort alpha emitters back-to-front
    std::sort(alpha_indices.begin(), alpha_indices.end(), [&](uint32_t a, uint32_t b) {
        glm::vec3 da = emitter_data_cache_[a].emitter_position - camera_pos_;
        glm::vec3 db = emitter_data_cache_[b].emitter_position - camera_pos_;
        return glm::dot(da, da) > glm::dot(db, db);
    });
    draw_order.insert(draw_order.end(), alpha_indices.begin(), alpha_indices.end());

    gpu::GraphicsPipeline bound{};
    for (uint32_t idx : draw_order) {
        if (emitters_[idx].is_mesh) continue; // mesh particles drawn separately

        const bool additive = emitters_[idx].blend_mode == ParticleBlendMode::additive;
        const bool lit = emitters_[idx].is_lit;
        const bool trail = emitters_[idx].is_trail;
        gpu::GraphicsPipeline needed{};
        if (lit) {
            if (trail) needed = additive ? pipelines_.lit_trail_additive : pipelines_.lit_trail_alpha;
            else       needed = additive ? pipelines_.lit_additive : pipelines_.lit_alpha;
        } else {
            if (trail) needed = additive ? pipelines_.trail_additive : pipelines_.trail_alpha;
            else       needed = additive ? pipelines_.additive : pipelines_.alpha;
        }
        if (needed != bound) {
            use(ctx, pass, needed);
            bound = needed;
        }
        if (lit) {
            pass.push(ParticleLitPush{emitters_[idx].lit_metallic, emitters_[idx].lit_roughness,
                                      emitters_[idx].mesh_material_id});
        }
        pass.draw_indirect(gpu::BufferRange(indirect_, idx * sizeof(gpu::DrawArgs), sizeof(gpu::DrawArgs)), 1,
                           sizeof(gpu::DrawArgs));
    }

    // ── Mesh particle rendering ──
    // Upload primitives lazily on first use
    if (ctx.mega_buffer && !primitives_uploaded_) {
        upload_primitives(*ctx.mega_buffer);
    }

    bool any_mesh = false;
    for (const auto& em : emitters_) {
        if (em.is_mesh && draws_in(em, ctx)) { any_mesh = true; break; }
    }
    if (!any_mesh || !ctx.mega_buffer) return;

    bound = {};
    for (uint32_t idx : draw_order) {
        if (!emitters_[idx].is_mesh) continue;

        const bool additive = emitters_[idx].blend_mode == ParticleBlendMode::additive;
        const bool lit = emitters_[idx].is_lit;
        const gpu::GraphicsPipeline needed =
            lit ? (additive ? pipelines_.mesh_lit_additive : pipelines_.mesh_lit_alpha)
                : (additive ? pipelines_.mesh_additive : pipelines_.mesh_alpha);
        if (needed != bound) {
            use(ctx, pass, needed);
            pass.set_vertex_buffer(ctx.mega_buffer->vertex_buffer());
            pass.set_index_buffer(ctx.mega_buffer->index_buffer(), gpu::IndexType::u32);
            bound = needed;
        }
        if (lit) {
            pass.push(ParticleLitPush{emitters_[idx].lit_metallic, emitters_[idx].lit_roughness,
                                      emitters_[idx].mesh_material_id});
        }
        // particle_compact.comp compacted this emitter's alive particles
        // into a dense GpuMeshInstance range and wrote the indexed draw for
        // them: the vertex shader reads gl_InstanceIndex straight out of
        // that range, so only live particles are drawn.
        pass.draw_indexed_indirect(
            gpu::BufferRange(mesh_indirect_, idx * sizeof(gpu::DrawIndexedArgs), sizeof(gpu::DrawIndexedArgs)),
            1, sizeof(gpu::DrawIndexedArgs));
    }
}

void ParticleSystem::record_render_opaque(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    FJELL_PROFILE_SCOPE_N("particle_record_render_opaque");
    if (emitters_.empty() || !emitter_records_.buffer.valid()) return;

    // Check if any opaque emitters exist
    bool any_opaque = false;
    for (const auto& em : emitters_) {
        if (draws_in(em, ctx) && em.blend_mode == ParticleBlendMode::opaque) { any_opaque = true; break; }
    }
    if (!any_opaque) return;
    auto zone = ctx.commands().zone("Particles: Opaque");
    // The opaque shaders read no depth: they blend nothing, so they have no
    // soft fade, and they write the depth this pass draws into.

    if (ctx.mega_buffer && !primitives_uploaded_) {
        upload_primitives(*ctx.mega_buffer);
    }

    gpu::GraphicsPipeline bound{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(emitters_.size()); ++i) {
        if (!draws_in(emitters_[i], ctx)) continue;
        if (emitters_[i].blend_mode != ParticleBlendMode::opaque) continue;

        const bool lit = emitters_[i].is_lit;
        const bool mesh = emitters_[i].is_mesh && ctx.mega_buffer != nullptr;
        const gpu::GraphicsPipeline needed = mesh ? (lit ? pipelines_.mesh_opaque_lit : pipelines_.mesh_opaque)
                                                  : (lit ? pipelines_.opaque_lit : pipelines_.opaque);
        if (needed != bound) {
            use(ctx, pass, needed);
            if (mesh) {
                pass.set_vertex_buffer(ctx.mega_buffer->vertex_buffer());
                pass.set_index_buffer(ctx.mega_buffer->index_buffer(), gpu::IndexType::u32);
            }
            bound = needed;
        }
        if (lit) {
            pass.push(ParticleLitPush{emitters_[i].lit_metallic, emitters_[i].lit_roughness,
                                      emitters_[i].mesh_material_id});
        }
        if (mesh) {
            // See record_render's mesh loop: the same compacted-instance
            // draw, in the opaque pass.
            pass.draw_indexed_indirect(
                gpu::BufferRange(mesh_indirect_, i * sizeof(gpu::DrawIndexedArgs), sizeof(gpu::DrawIndexedArgs)),
                1, sizeof(gpu::DrawIndexedArgs));
        } else {
            pass.draw_indirect(gpu::BufferRange(indirect_, i * sizeof(gpu::DrawArgs), sizeof(gpu::DrawArgs)), 1,
                               sizeof(gpu::DrawArgs));
        }
    }
}

// ── VFX hot-reload (debug builds only) ──────────────────────────────────

#ifndef NDEBUG
void ParticleSystem::setup_vfx_watcher(const RenderSubsystemContext& ctx) {
    if (!ctx.scene || !ctx.project) return;
    if (ctx.scene == watched_scene_) return;
    watched_scene_ = ctx.scene;

    vfx_watcher_.clear();
    vfx_changed_ = false;

    std::unordered_set<std::string> watched_dirs;
    for (auto [obj, ec] : ctx.scene->view<EmitterComponent>()) {
        if (ec->vfx_path.empty()) continue;
        auto abs = ctx.project->resolve(ec->vfx_path);
        auto dir = std::filesystem::path(abs).parent_path().string();
        if (watched_dirs.contains(dir)) continue;
        watched_dirs.insert(dir);
        vfx_watcher_.watch(dir, [this](const std::filesystem::path& p) {
            if (p.extension() == ".fjvfx") {
                vfx_changed_ = true;
                vfx_changed_path_ = p.string();
                vfx_relative_path_.clear();
                vfx_reloaded_scenes_.clear();
            }
        });
    }
}

void ParticleSystem::reload_changed_vfx(const RenderSubsystemContext& ctx) {
    if (vfx_changed_path_.empty()) return;

    // Already applied to this scene
    if (vfx_reloaded_scenes_.contains(ctx.scene)) return;
    vfx_reloaded_scenes_.insert(ctx.scene);

    if (vfx_relative_path_.empty()) {
        auto root = ctx.project->root().string();
        if (vfx_changed_path_.starts_with(root)) {
            vfx_relative_path_ = vfx_changed_path_.substr(root.size());
            if (!vfx_relative_path_.empty() && vfx_relative_path_[0] == '/')
                vfx_relative_path_ = vfx_relative_path_.substr(1);
        }
    }

    // Each node gets its own copy: an emitter's playback state (its clock,
    // accumulators, whether its burst has fired) lives on the effect, and
    // two nodes sharing one would advance it twice a frame and fire once
    // between them.
    int replaced = 0;
    for (auto [obj, ec] : ctx.scene->view<EmitterComponent>()) {
        if (ec->vfx_path != vfx_relative_path_ && ec->vfx_path != vfx_changed_path_) continue;
        try {
            auto effect = load_vfx_effect(vfx_changed_path_);
            if (ctx.resource_manager)
                resolve_vfx_textures(effect, *ctx.resource_manager, ctx.project);
            ec->effect = std::make_shared<VfxEffect>(std::move(effect));
            replaced++;
        } catch (const std::exception& e) {
            FJELL_CORE_ERROR("VFX hot-reload failed: {}", e.what());
            // Clear state so we don't retry every frame
            vfx_changed_ = false;
            vfx_changed_path_.clear();
            return;
        }
    }

    if (replaced > 0) {
        FJELL_CORE_INFO("VFX hot-reloaded: {} ({} instance(s))", vfx_relative_path_, replaced);
    }
}
#endif

} // namespace fjell
