#include "renderer/accel_structure.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/systems/particle_system.hpp"
#include "scene/scene.hpp"

#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/device.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>

namespace fjell {

namespace {

// particle_tlas_build.comp's push constants: one mesh particle emitter.
struct ParticleTlasPush {
    uint32_t particle_offset;
    uint32_t particle_count;
    uint32_t material_id;
    float particle_size;
    uint32_t blas_reference_lo;
    uint32_t blas_reference_hi;
    uint32_t first_index;
    uint32_t vertex_offset;
    uint32_t mask;
};

// The triangles of one mesh entry, as a BLAS is built from them.
gpu::Triangles entry_triangles(const MegaMeshEntry& entry, const MegaBuffer& mega) {
    return {
        .vertices = gpu::BufferRange(mega.vertex_buffer(), uint64_t{entry.vertex_offset} * sizeof(Vertex),
                                     uint64_t{entry.vertex_count} * sizeof(Vertex)),
        .vertex_stride = sizeof(Vertex),
        .vertex_count = entry.vertex_count,
        .indices = gpu::BufferRange(mega.index_buffer(), uint64_t{entry.first_index} * sizeof(uint32_t),
                                    uint64_t{entry.index_count} * sizeof(uint32_t)),
        .triangle_count = entry.index_count / 3,
    };
}

// Whether a transform places anything a TLAS can hold. A just-spawned
// networked object can reach the render queue before its world matrix is
// computed (NaN or zero), and a broken instance transform makes the driver
// build a malformed TLAS that faults on traversal, losing the device. Such
// an instance is left out this frame and appears the next.
bool placeable(const glm::mat4& m) {
    const bool finite = std::isfinite(m[0][0]) && std::isfinite(m[1][1]) && std::isfinite(m[2][2]) &&
                        std::isfinite(m[3][0]) && std::isfinite(m[3][1]) && std::isfinite(m[3][2]);
    // A zero upper-left 3x3 is zero scale: an invalid box.
    const float scale = std::abs(m[0][0]) + std::abs(m[1][1]) + std::abs(m[2][2]) + std::abs(m[0][1]) +
                        std::abs(m[0][2]) + std::abs(m[1][0]) + std::abs(m[1][2]) + std::abs(m[2][0]) +
                        std::abs(m[2][1]);
    return finite && scale >= 1e-12F;
}

// A listed instance as an instance record's source: the transform's three
// rows, the material as the custom index.
gpu::AccelerationInstance record_of(const RtInstance& instance) {
    const glm::mat4& m = instance.transform;
    gpu::AccelerationInstance record{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            record.transform[static_cast<size_t>(row * 4 + column)] = m[column][row];
        }
    }
    record.custom_index = instance.material_id;
    record.mask = static_cast<uint8_t>(instance.mask);
    record.structure = instance.blas;
    return record;
}

} // namespace

AccelStructureManager::AccelStructureManager(gpu::Device& device) : device_(device) {
    auto made = device_.create(gpu::ComputePipelineDesc{
        .shader = "shaders/particle_tlas_build.comp",
        .name = "particle_tlas_build",
    });
    if (made) {
        particle_pipeline_ = std::move(*made);
    } else {
        FJELL_GFX_ERROR("Mesh particles are left out of ray tracing: {}", made.error());
    }
}

bool AccelStructureManager::declare_trace(PassBuilder& builder, const DeclareContext& ctx) {
    const FgAcceleration tlas = builder.import_named_acceleration(ctx, TLAS_NAME);
    builder.read(tlas, gpu::Access::acceleration_trace_compute);
    builder.read(builder.import_named_buffer(ctx, INSTANCES_NAME), gpu::Access::storage_buffer_read_compute);
    return tlas.valid();
}

std::vector<Blas> AccelStructureManager::make_blases(std::span<const MegaMeshEntry> entries,
                                                      const MegaBuffer& mega) {
    std::vector<Blas> out(entries.size());
    // Every structure of the batch shares one allocation, so a model with
    // many submeshes still costs one.
    std::vector<gpu::AccelerationStructureDesc> descs;
    std::vector<size_t> made_for;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].index_count < 3) continue;
        descs.push_back({.triangles = entry_triangles(entries[i], mega), .name = "blas"});
        made_for.push_back(i);
    }
    if (descs.empty()) return out;

    auto made = device_.create(descs);
    if (!made) {
        FJELL_GFX_ERROR("Ray tracing leaves a mesh out: {}", made.error());
        return out;
    }
    for (size_t k = 0; k < made_for.size(); ++k) {
        const MegaMeshEntry& entry = entries[made_for[k]];
        Blas& blas = out[made_for[k]];
        blas.structure = std::move((*made)[k]);
        blas.first_index = entry.first_index;
        blas.vertex_offset = entry.vertex_offset;
        pending_builds_.push_back({blas.structure, descs[k].triangles});
    }
    return out;
}

void AccelStructureManager::ensure_blas(const Model* model, const MegaBuffer& mega) {
    // A BLAS keeps nothing of the buffers it was built from, so the
    // mega-buffer moving to grow leaves it whole.
    if (blas_map_.contains(model)) return;

    // LOD 0 only: rays want the highest-quality geometry regardless of what
    // the raster path picked for this frame.
    const auto& lods = model->lod_levels();
    if (lods.empty()) return;
    std::span<const MegaMeshEntry> all = model->mega_entries();
    if (lods[0].first_mega_entry + lods[0].mega_entry_count > all.size()) return;
    auto made = make_blases(all.subspan(lods[0].first_mega_entry, lods[0].mega_entry_count), mega);
    if (!made.empty()) blas_map_[model] = std::move(made);
}

void AccelStructureManager::ensure_blas_entry(const void* key, const MegaMeshEntry& entry,
                                              const MegaBuffer& mega, bool geometry_changed) {
    if (auto it = entry_blas_map_.find(key); it != entry_blas_map_.end()) {
        if (!geometry_changed) return;
        // Made anew rather than built over: a frame still tracing the old
        // one keeps it until it is done.
        const gpu::AccelerationStructure old = it->second.structure;
        std::erase_if(pending_builds_, [old](const PendingBuild& build) { return build.blas == old; });
        entry_blas_map_.erase(it);
    }
    auto made = make_blases(std::span<const MegaMeshEntry>{&entry, 1}, mega);
    if (!made.empty() && made.front().structure) {
        entry_blas_map_[key] = std::move(made.front());
    }
}

const Blas* AccelStructureManager::find_blas(const void* key) const {
    auto it = entry_blas_map_.find(key);
    return it != entry_blas_map_.end() ? &it->second : nullptr;
}

std::span<const Blas> AccelStructureManager::model_blases(const Model* model) const {
    auto it = blas_map_.find(model);
    if (it == blas_map_.end()) return {};
    return it->second;
}

void AccelStructureManager::begin_frame(uint64_t frame_serial) {
    frame_serial_ = frame_serial;

    // A scene nothing has prepared for a few frames (a scene loaded over it,
    // a preview closed) gives its structures back, once the frames that may
    // still trace them are done.
    std::erase_if(scenes_, [&](const auto& entry) {
        return entry.second.prepared_serial + MAX_FRAMES_IN_FLIGHT + 1 < frame_serial_;
    });
}

void AccelStructureManager::prepare_scene(const Scene& scene, std::span<const DrawCommand> surfaces,
                                          const PipelineCache* pipelines,
                                          const ParticleSystem* particles, MegaBuffer& mega,
                                          uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("rt_prepare_scene");
    SceneTlas& st = scenes_[scene.id()];
    st.prepared_serial = frame_serial_;
    st.host_instances.clear();
    st.particle_emitters.clear();
    st.instance_count = 0;

    const auto list = [&st](RtInstance instance) {
        if (placeable(instance.transform)) st.host_instances.push_back(instance);
    };

    // One instance per submesh a surface draws, each over that submesh's own
    // BLAS, so a hit reports the triangle against the material and index
    // range of the geometry it actually struck.
    for (const DrawCommand& dc : surfaces) {
        if (!dc.model) continue;
        ensure_blas(dc.model, mega);
        const auto& lods = dc.model->lod_levels();
        if (lods.empty()) continue;
        const std::span<const Blas> blases = model_blases(dc.model);
        const bool two_sided = pipelines != nullptr
                            && pipelines->cull(dc.pipeline_id) != gpu::Cull::back;
        for (uint32_t i = 0; i < blases.size(); ++i) {
            if (!blases[i].structure) continue;
            if (!dc.model->entry_uses_slot(lods[0].first_mega_entry + i, dc.material_slot)) continue;

            RtInstance inst{};
            inst.transform = dc.push.model;
            inst.material_id = dc.push.material_id;
            inst.blas = blases[i].structure;
            inst.first_index = blases[i].first_index;
            inst.vertex_offset = blases[i].vertex_offset;
            inst.mask = rt_instance_mask(dc.transparent);
            inst.two_sided = two_sided;
            list(inst);
        }
    }

    // Terrain generates its render geometry in a mesh shader and never exists
    // as triangles, so rays trace the decimated mesh the asset builds from
    // its height array.
    for (auto [obj, tc] : scene.view<TerrainComponent>()) {
        if (tc->terrain == nullptr) continue;
        TerrainAsset& terrain = *tc->terrain;
        terrain.upload_ray_mesh(mega);
        if (!terrain.ray_mesh_uploaded()) continue;
        // A sculpt moves the ground the rays trace against. The mesh keeps
        // its place in the buffer — same grid, new heights — and its new
        // vertices land before this frame's builds, as every upload does.
        const bool moved = terrain.refresh_ray_mesh(mega);
        ensure_blas_entry(&terrain, terrain.ray_mesh_entry(), mega, moved);
        const Blas* blas = find_blas(&terrain);
        if (blas == nullptr) continue;

        const auto* spatial = obj->as_spatial();
        RtInstance inst{};
        // Translation only, as the raster path and physics place it: the
        // terrain's shape is its asset's, sampled along world X and Z, and
        // the node only says where. Passing the full matrix here would rotate
        // what rays hit away from what is drawn.
        inst.transform = glm::translate(glm::mat4{1.0F},
                                        spatial != nullptr ? glm::vec3{spatial->world_matrix_[3]}
                                                           : glm::vec3{0.0F});
        // Rays cannot run the layer blend, so the terrain bounces its first
        // layer, which is the ground where nothing has been painted or
        // sloped into another.
        inst.material_id = terrain.layer_material_id(0);
        inst.blas = blas->structure;
        inst.first_index = blas->first_index;
        inst.vertex_offset = blas->vertex_offset;
        inst.mask = RT_MASK_OPAQUE;
        list(inst);
    }

    // Mesh particles: their instances are written on the GPU from the
    // simulated particles, over one BLAS per mesh shape.
    uint32_t particle_room = 0;
    particle_state_ = particles != nullptr ? particles->particle_buffer() : gpu::Buffer{};
    if (particle_state_.valid() && particle_pipeline_) {
        for (const auto& em : particles->emitters()) {
            if (!em.active || !em.is_mesh || em.scene_id != scene.id()) continue;
            const auto* shape_key = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(em.mesh_entry.first_index + 1));
            ensure_blas_entry(shape_key, em.mesh_entry, mega);
            const Blas* blas = find_blas(shape_key);
            if (blas == nullptr) continue;

            ParticleEmitterInfo info{};
            info.base_offset = em.base_offset;
            info.max_particles = em.max_particles;
            info.material_id = em.mesh_material_id;
            info.avg_particle_size = 1.0F;
            info.blas_reference = device_.instance_reference(blas->structure);
            info.first_index = blas->first_index;
            info.vertex_offset = blas->vertex_offset;
            info.mask = rt_instance_mask(em.blend_mode != ParticleBlendMode::opaque);
            st.particle_emitters.push_back(info);
            particle_room += em.max_particles;
        }
    }

    const uint32_t count = static_cast<uint32_t>(st.host_instances.size()) + particle_room;
    if (count == 0) return;
    if (reserve(st.frames[frame_index], count)) st.instance_count = count;
}

bool AccelStructureManager::reserve(FrameTlas& frame, uint32_t count) {
    if (frame.tlas && frame.capacity >= count) return true;
    // Grown with room to spare, so a scene gaining a few instances a frame
    // is not made anew each time. The old ones stay with the frames still
    // tracing them.
    const uint32_t capacity = std::max(count, frame.capacity + frame.capacity / 2);
    auto tlas = device_.create(gpu::AccelerationStructureDesc{
        .instances = capacity, .use = gpu::AccelerationUse::rebuilt, .name = "tlas"});
    auto instances = device_.create(gpu::BufferDesc{
        .size = uint64_t{capacity} * device_.instance_record_size(),
        .use = gpu::BufferUse::acceleration_input | gpu::BufferUse::storage,
        .memory = gpu::Memory::upload,
        .name = "tlas instances",
    });
    auto rt_data = device_.create(gpu::BufferDesc{
        .size = uint64_t{capacity} * sizeof(GpuRtInstance),
        .use = gpu::BufferUse::storage,
        .memory = gpu::Memory::upload,
        .name = "rt instance data",
    });
    auto counter = device_.create(gpu::BufferDesc{
        .size = sizeof(uint32_t), .use = gpu::BufferUse::storage, .name = "particle instance count"});
    if (!tlas || !instances || !rt_data || !counter) {
        FJELL_GFX_ERROR("Ray tracing has no TLAS for {} instances: {}", count,
                        !tlas ? tlas.error()
                              : !instances ? instances.error()
                                           : !rt_data ? rt_data.error() : counter.error());
        return false;
    }
    frame.tlas = std::move(*tlas);
    frame.instances = std::move(*instances);
    frame.rt_instance_data = std::move(*rt_data);
    frame.particle_counter = std::move(*counter);
    frame.capacity = capacity;
    frame.reusable = false;
    return true;
}

uint32_t AccelStructureManager::record_build(gpu::CommandList& commands, uint64_t scene_id,
                                             uint32_t frame_index) {
    using gpu::Access;

    // Every BLAS made since the last build, whichever scene lists it, so
    // none waits on a scene's TLAS for its first build.
    if (!pending_builds_.empty()) {
        std::vector<gpu::AccelerationStructure> built;
        built.reserve(pending_builds_.size());
        for (const PendingBuild& pending : pending_builds_) {
            commands.build(pending.blas, pending.triangles);
            built.push_back(pending.blas);
        }
        pending_builds_.clear();
        commands.barrier(built, Access::acceleration_build, Access::acceleration_build_input);
    }

    auto it = scenes_.find(scene_id);
    if (it == scenes_.end()) return 0;
    SceneTlas& st = it->second;
    if (st.prepared_serial != frame_serial_ || st.instance_count == 0) return 0;
    FrameTlas& frame = st.frames[frame_index];
    if (frame.current_serial == frame_serial_) return 0;
    if (frame.reusable && frame.built_from == st.host_instances && st.particle_emitters.empty()) {
        frame.current_serial = frame_serial_;
        return frame.instance_count;
    }
    return build_tlas(commands, st, frame);
}

uint32_t AccelStructureManager::build_tlas(gpu::CommandList& commands, SceneTlas& scene,
                                           FrameTlas& frame) {
    using gpu::Access;
    const std::vector<RtInstance>& listed = scene.host_instances;
    const auto scene_count = static_cast<uint32_t>(listed.size());
    const uint32_t record_size = device_.instance_record_size();

    // The listed instances' records at the front, and beside each the data
    // the hit code reads by the instance's index: the two must agree in
    // count and order. Written by the CPU, seen by work submitted after.
    std::vector<gpu::AccelerationInstance> records;
    std::vector<GpuRtInstance> rt_instances;
    records.reserve(listed.size());
    rt_instances.reserve(listed.size());
    for (const RtInstance& instance : listed) {
        records.push_back(record_of(instance));
        rt_instances.push_back({.material_id = instance.material_id,
                                .first_index = instance.first_index,
                                .vertex_offset = instance.vertex_offset,
                                .flags = instance.two_sided ? RT_INSTANCE_TWO_SIDED : 0U});
    }
    device_.write_instances(device_.mapped(frame.instances), records);
    if (!rt_instances.empty()) {
        std::memcpy(device_.mapped(frame.rt_instance_data).data(), rt_instances.data(),
                    rt_instances.size() * sizeof(GpuRtInstance));
    }

    // Mesh particles, written by the GPU after the listed ones: their count
    // is known only once it has culled the dead. The rest of their room is
    // records naming nothing, which no ray hits, and zeroed hit data.
    const uint32_t particle_room = scene.instance_count - scene_count;
    if (particle_room > 0) {
        const gpu::BufferRange particle_records(frame.instances, uint64_t{scene_count} * record_size,
                                                uint64_t{particle_room} * record_size);
        const gpu::BufferRange particle_data(frame.rt_instance_data, uint64_t{scene_count} * sizeof(GpuRtInstance),
                                             uint64_t{particle_room} * sizeof(GpuRtInstance));
        commands.fill(particle_records, 0);
        commands.fill(particle_data, 0);
        // They append after the listed instances.
        commands.fill(frame.particle_counter, scene_count);
        commands.barrier(particle_records, Access::clear, Access::storage_buffer_write_compute);
        commands.barrier(particle_data, Access::clear, Access::storage_buffer_write_compute);
        commands.barrier(frame.particle_counter, Access::clear, Access::storage_buffer_read_write_compute);

        commands.set_pipeline(particle_pipeline_);
        commands.bind({{"ParticleBuffer", gpu::storage(particle_state_)},
                       {"InstanceBuffer", gpu::storage(frame.instances)},
                       {"CounterBuffer", gpu::storage(frame.particle_counter)},
                       {"RtInstanceBuffer", gpu::storage(frame.rt_instance_data)}});
        for (const ParticleEmitterInfo& pe : scene.particle_emitters) {
            commands.push(ParticleTlasPush{
                .particle_offset = pe.base_offset,
                .particle_count = pe.max_particles,
                .material_id = pe.material_id,
                .particle_size = pe.avg_particle_size,
                .blas_reference_lo = static_cast<uint32_t>(pe.blas_reference & 0xFFFFFFFFU),
                .blas_reference_hi = static_cast<uint32_t>(pe.blas_reference >> 32),
                .first_index = pe.first_index,
                .vertex_offset = pe.vertex_offset,
                .mask = pe.mask,
            });
            commands.dispatch((pe.max_particles + 63) / 64, 1, 1);
        }
        // The build reads the records the particles wrote; the hit data
        // is ordered for the tracers by the graph.
        commands.barrier(particle_records, Access::storage_buffer_write_compute,
                         Access::acceleration_build_input);
    }

    commands.build(frame.tlas, gpu::BufferRange(frame.instances, 0, uint64_t{scene.instance_count} * record_size),
                   scene.instance_count);

    frame.built_from = listed;
    frame.reusable = scene.particle_emitters.empty();
    frame.instance_count = scene.instance_count;
    frame.current_serial = frame_serial_;
    return scene.instance_count;
}

gpu::AccelerationStructure AccelStructureManager::prepared_tlas(uint64_t scene_id,
                                                                uint32_t frame_index) const {
    auto it = scenes_.find(scene_id);
    if (it == scenes_.end() || it->second.prepared_serial != frame_serial_ ||
        it->second.instance_count == 0) {
        return {};
    }
    return it->second.frames[frame_index].tlas;
}

gpu::Buffer AccelStructureManager::prepared_instances(uint64_t scene_id, uint32_t frame_index) const {
    if (!prepared_tlas(scene_id, frame_index).valid()) return {};
    return scenes_.at(scene_id).frames[frame_index].rt_instance_data;
}

gpu::AccelerationStructure AccelStructureManager::tlas(uint64_t scene_id, uint32_t frame_index) const {
    auto it = scenes_.find(scene_id);
    if (it == scenes_.end()) return {};
    const FrameTlas& frame = it->second.frames[frame_index];
    return frame.current_serial == frame_serial_ ? gpu::AccelerationStructure(frame.tlas)
                                                 : gpu::AccelerationStructure{};
}

gpu::Buffer AccelStructureManager::instance_data_buffer(uint64_t scene_id, uint32_t frame_index) const {
    auto it = scenes_.find(scene_id);
    if (it == scenes_.end()) return {};
    const FrameTlas& frame = it->second.frames[frame_index];
    return frame.current_serial == frame_serial_ ? gpu::Buffer(frame.rt_instance_data) : gpu::Buffer{};
}

} // namespace fjell
