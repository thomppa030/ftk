#include "renderer/pass/scene_sdf_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/terrain_asset.hpp"

#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/device.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fjell {

namespace {

// The slice shader's push constants (sdf_slice.comp).
struct SlicePush {
    uint32_t axis;
    float slice_pos;
    uint32_t resolution;
    float max_distance;
    float surface;
    uint32_t distance_offset;
};
static_assert(sizeof(SlicePush) == 24);

// The compose shader's push constants (sdf_composite.comp): one region.
struct ComposePush {
    glm::vec3 volume_min;
    uint32_t instance_offset;
    glm::vec3 volume_max;
    uint32_t instance_count;
    glm::ivec3 region_min;
    uint32_t resolution;
    glm::ivec3 region_extent;
    float max_distance;
};
static_assert(sizeof(ComposePush) == 64);

// Room in each frame slot's instance list.
constexpr uint32_t INSTANCE_CAPACITY = 1024;

// Room for every mesh's distance grid and terrain's heights: about 2000
// meshes at 32³.
constexpr uint64_t GRID_BUFFER_SIZE = 128ULL * 1024 * 1024;

// What the volumes hold before their first compose: nothing near, and no
// object named.
constexpr float EMPTY_DISTANCE = 65504.0F;  // the largest a half float holds

// Where the volumes rest between composes: sampled, through the global set,
// by fragment and compute shaders alike.
constexpr gpu::AccessSet SAMPLED = gpu::Access::sampled_fragment | gpu::Access::sampled_compute;

} // namespace

Result<> SceneSdfPass::create(const PassResources& resources) {
    device_ = resources.device;

    const auto volume_desc = [&](gpu::Format format, float empty, std::string_view name) {
        return gpu::TextureDesc{
            .kind = gpu::TextureKind::tex3d,
            .format = format,
            .width = resolution_,
            .height = resolution_,
            .depth = resolution_,
            .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
            .initial = gpu::Clear{empty},
            .name = name,
        };
    };
    FJELL_TRY(volume_, device_->create(volume_desc(SDF_FORMAT, EMPTY_DISTANCE, "scene_sdf")));
    FJELL_TRY(index_volume_,
              device_->create(volume_desc(SDF_INDEX_FORMAT, static_cast<float>(NO_SDF_SLOT), "scene_sdf_index")));
    volume_sampler_ = device_->sampler({.filter = gpu::Filter::linear,
                                        .mip_filter = gpu::Filter::nearest,
                                        .address = gpu::Address::clamp});
    index_sampler_ = device_->sampler({.filter = gpu::Filter::nearest,
                                       .mip_filter = gpu::Filter::nearest,
                                       .address = gpu::Address::clamp});

    FJELL_TRY(composite_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                       .shader = "shaders/sdf_composite.comp",
                                       .name = "sdf_composite",
                                   }));

    FJELL_TRY(mesh_sdf_buffer_, device_->create(gpu::BufferDesc{
                                    .size = GRID_BUFFER_SIZE,
                                    .use = gpu::BufferUse::storage,
                                    .memory = gpu::Memory::upload,
                                    .name = "sdf grids",
                                }));
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        FJELL_TRY(instance_buffers_[slot], device_->create(gpu::BufferDesc{
                                               .size = INSTANCE_CAPACITY * sizeof(GpuSdfInstance),
                                               .use = gpu::BufferUse::storage,
                                               .memory = gpu::Memory::upload,
                                               .name = "sdf instances",
                                           }));
        FJELL_TRY(slot_tables_[slot], device_->create(gpu::BufferDesc{
                                          .size = MAX_SDF_SLOTS * sizeof(GpuSdfSlot),
                                          .use = gpu::BufferUse::storage,
                                          .memory = gpu::Memory::upload,
                                          .name = "sdf objects",
                                      }));
    }

    FJELL_GFX_DEBUG("SceneSdfPass created ({}³ R16_SFLOAT, {:.1f}MB)", resolution_,
                    float(resolution_ * resolution_ * resolution_ * 2) / (1024.0f * 1024.0f));
    return {};
}

void SceneSdfPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    gpu.global_bindings().set("scene_sdf", gpu::sampled(volume_, volume_sampler_));
}

void SceneSdfPass::destroy() {
    slice_pipeline_.reset();
    slice_texture_.reset();
    slice_distances_.reset();
    slice_written_.fill(false);

    mesh_sdf_buffer_.reset();
    mesh_sdf_offset_ = 0;
    model_sdf_entries_.clear();
    terrain_sdf_entries_.clear();
    for (auto& buffer : instance_buffers_) buffer.reset();
    for (auto& table : slot_tables_) table.reset();
    slots_.clear();
    free_slots_.clear();
    tracked_.clear();
    gathered_.clear();
    gathered_bounds_.clear();
    dirty_regions_.clear();
    full_rebuild_ = true;
    composed_ = false;

    composite_pipeline_.reset();
    volume_.reset();
    index_volume_.reset();
}

PassSetup SceneSdfPass::setup() const {
    return {
        .name = "scene_sdf",
        // First of all: much of what reads the field does so through the
        // global set, which the graph cannot order after this.
        .phase = RenderPhase::Environment,
        .sub_order = 0,
    };
}

void SceneSdfPass::collect_exports(DeclareContext& ctx) {
    const TextureExport sampled{.persistent = true, .resting = SAMPLED};
    ctx.export_texture(VOLUME_NAME, volume_, sampled);
    ctx.export_texture(INDEX_NAME, index_volume_, sampled);
    // ImGui shows the slice.
    if (slice_texture_) {
        ctx.export_texture(SLICE_NAME, slice_texture_,
                           {.persistent = true, .resting = gpu::Access::sampled_fragment});
    }
}

void SceneSdfPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    const bool compose_now = has_pending_update();
    const bool slice_now = slice_requested_ && slice_texture_ && (compose_now || composed_);
    if (!compose_now && !slice_now) return;
    const FgTexture volume = builder.import_named(ctx, VOLUME_NAME);
    if (compose_now) {
        const FgTexture index = builder.import_named(ctx, INDEX_NAME);
        if (full_rebuild_) {
            builder.write(volume, gpu::Access::storage_write_compute);
            builder.write(index, gpu::Access::storage_write_compute);
        } else {
            // A region's compose keeps every voxel outside it.
            builder.read_write(volume, gpu::Access::storage_read_write_compute);
            builder.read_write(index, gpu::Access::storage_read_write_compute);
        }
        // The slice after it reads the distances the compose wrote.
        if (slice_now) {
            builder.leaves(volume, full_rebuild_ ? gpu::Access::storage_write_compute
                                                 : gpu::Access::storage_read_write_compute,
                           gpu::Access::sampled_compute);
        }
    } else if (slice_now) {
        builder.read(volume, gpu::Access::sampled_compute);
    }
    if (slice_now) {
        builder.write(builder.import_named(ctx, SLICE_NAME), gpu::Access::storage_write_compute);
    }
}

void SceneSdfPass::record(const FrameContext& ctx) {
    // Once a frame: the first viewport composes what is pending and draws
    // the slice asked for; those after find nothing to do.
    if (!has_pending_update() && !slice_requested_) return;
    FJELL_PROFILE_SCOPE_N("record_scene_sdf");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Scene SDF");
    const bool composing = has_pending_update();
    const bool full = full_rebuild_;
    if (composing) compose(cmd, ctx.frame_index);
    if (slice_requested_ && slice_texture_ && composed_) {
        // The distances just written, as the slice samples them; declared
        // as what the pass leaves them in.
        if (composing) {
            cmd.barrier(volume_, full ? gpu::Access::storage_write_compute : gpu::Access::storage_read_write_compute,
                        gpu::Access::sampled_compute);
        }
        draw_slice(cmd, ctx.frame_index);
    }
    slice_requested_ = false;
}

bool SceneSdfPass::register_model_sdf(const Model& model, uintptr_t model_id) {
    if (!model.has_sdf()) return false;
    if (model_sdf_entries_.count(model_id)) return true; // already registered

    uint32_t res = model.sdf_grid_resolution();
    uint32_t voxel_count = res * res * res;
    const auto& data = model.sdf_grid_data();
    if (data.size() != voxel_count) return false;

    // Check buffer capacity
    uint32_t bytes_needed = voxel_count * sizeof(int16_t);
    uint32_t current_bytes = mesh_sdf_offset_ * sizeof(int16_t);
    if (current_bytes + bytes_needed > device_->size(mesh_sdf_buffer_)) {
        FJELL_GFX_WARN("SDF mesh buffer full, cannot register model {}", model_id);
        return false;
    }

    // Upload to SSBO
    auto* dst = reinterpret_cast<int16_t*>(device_->mapped(mesh_sdf_buffer_).data()) + mesh_sdf_offset_;
    std::memcpy(dst, data.data(), bytes_needed);

    // Compute max_dist for denormalization
    glm::vec3 extent = model.sdf_volume_max() - model.sdf_volume_min();
    float max_dist = glm::length(extent);

    MeshSdfEntry entry{};
    entry.offset = mesh_sdf_offset_;
    entry.resolution = res;
    entry.sdf_min = model.sdf_volume_min();
    entry.sdf_max = model.sdf_volume_max();
    entry.mesh_min = model.local_aabb().min;
    entry.mesh_max = model.local_aabb().max;
    entry.max_dist = max_dist;
    model_sdf_entries_[model_id] = entry;

    mesh_sdf_offset_ += voxel_count;

    FJELL_GFX_DEBUG("Registered SDF for model {} ({}³, offset={})",
                    model_id, res, entry.offset);
    return true;
}

namespace {

// Heights are stored normalized in [0,1] and read back through the same
// int16 path as a mesh grid, which divides by 32767.
int16_t height_to_packed(float h) {
    return static_cast<int16_t>(std::lround(std::clamp(h, 0.0F, 1.0F) * 32767.0F));
}

} // namespace

const SceneSdfPass::TerrainSdfEntry* SceneSdfPass::register_terrain_sdf(
    const TerrainAsset& terrain) {
    auto it = terrain_sdf_entries_.find(&terrain);
    if (it != terrain_sdf_entries_.end()) return &it->second;

    const uint32_t samples = terrain.sample_count();
    const auto& heights = terrain.data().heights;
    const size_t count = static_cast<size_t>(samples) * samples;
    if (samples < 2 || heights.size() != count) return nullptr;

    const size_t bytes_needed = count * sizeof(int16_t);
    const size_t current_bytes = static_cast<size_t>(mesh_sdf_offset_) * sizeof(int16_t);
    if (current_bytes + bytes_needed > device_->size(mesh_sdf_buffer_)) {
        FJELL_GFX_WARN("SDF grid buffer full, terrain '{}' will not be part of the scene SDF",
                       terrain.path());
        return nullptr;
    }

    auto* dst = reinterpret_cast<int16_t*>(device_->mapped(mesh_sdf_buffer_).data()) + mesh_sdf_offset_;
    for (size_t i = 0; i < count; ++i) dst[i] = height_to_packed(heights[i]);

    TerrainSdfEntry entry{mesh_sdf_offset_, samples, terrain.edit_generation()};
    mesh_sdf_offset_ += static_cast<uint32_t>(count);
    FJELL_GFX_DEBUG("Registered terrain '{}' in the scene SDF ({}x{} samples, offset={})",
                    terrain.path(), samples, samples, entry.offset);
    return &terrain_sdf_entries_.emplace(&terrain, entry).first->second;
}

void SceneSdfPass::update_terrain_sdf(const TerrainAsset& terrain, uint32_t x, uint32_t z,
                                      uint32_t size_x, uint32_t size_z) {
    auto it = terrain_sdf_entries_.find(&terrain);
    if (it == terrain_sdf_entries_.end()) return;
    auto& entry = it->second;
    const uint32_t samples = entry.samples;
    if (samples != terrain.sample_count()) return;
    const auto& heights = terrain.data().heights;

    const uint32_t x_end = std::min(x + size_x, samples);
    const uint32_t z_end = std::min(z + size_z, samples);
    auto* dst = reinterpret_cast<int16_t*>(device_->mapped(mesh_sdf_buffer_).data()) + entry.offset;
    for (uint32_t row = z; row < z_end; ++row) {
        for (uint32_t col = x; col < x_end; ++col) {
            const size_t i = (static_cast<size_t>(row) * samples) + col;
            dst[i] = height_to_packed(heights[i]);
        }
    }
    entry.generation = terrain.edit_generation();
}

const SceneSdfPass::MeshSdfEntry* SceneSdfPass::model_entry(uintptr_t model_id) const {
    auto it = model_sdf_entries_.find(model_id);
    return it != model_sdf_entries_.end() ? &it->second : nullptr;
}

void SceneSdfPass::set_bounds(const glm::vec3& min, const glm::vec3& max) {
    if (volume_min_ != min || volume_max_ != max) {
        volume_min_ = min;
        volume_max_ = max;
        full_rebuild_ = true;
    }
}

glm::vec3 SceneSdfPass::voxel_size() const {
    return (volume_max_ - volume_min_) / float(resolution_);
}

float SceneSdfPass::max_distance() const {
    glm::vec3 v = voxel_size();
    return std::max(4.0f, 4.0f * std::max({v.x, v.y, v.z}));
}

void SceneSdfPass::begin_gather() {
    gathered_.clear();
    gathered_bounds_.clear();
    for (auto& [key, tracked] : tracked_) tracked.seen = false;
}

uint32_t SceneSdfPass::acquire_slot() {
    if (!free_slots_.empty()) {
        const uint32_t slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }
    if (slots_.size() >= MAX_SDF_SLOTS) return NO_SDF_SLOT;
    slots_.push_back({});
    return static_cast<uint32_t>(slots_.size() - 1);
}

void SceneSdfPass::gather_instance(uint64_t key, const AABB& world_bounds, bool moved,
                                   const GpuSdfInstance& instance,
                                   std::span<const uint32_t> materials) {
    auto it = tracked_.find(key);
    if (it == tracked_.end()) {
        add_dirty_region(world_bounds);
        it = tracked_.emplace(key, TrackedInstance{world_bounds, instance.grid_offset, true,
                                                   acquire_slot()})
                 .first;
    } else {
        auto& tracked = it->second;
        tracked.seen = true;
        if (moved || tracked.grid_offset != instance.grid_offset) {
            add_dirty_region(tracked.bounds);
            add_dirty_region(world_bounds);
            tracked.bounds = world_bounds;
            tracked.grid_offset = instance.grid_offset;
        }
    }

    const uint32_t slot = it->second.slot;
    GpuSdfInstance placed = instance;
    placed.slot = slot;
    gathered_.push_back(placed);
    gathered_bounds_.push_back(world_bounds);

    if (slot != NO_SDF_SLOT) {
        GpuSdfSlot& entry = slots_[slot];
        entry = {};
        entry.material_count = static_cast<uint32_t>(std::min<size_t>(materials.size(), SDF_SLOT_MATERIALS));
        for (uint32_t i = 0; i < entry.material_count; ++i) entry.material_ids[i] = materials[i];
    }
}

void SceneSdfPass::end_gather(uint32_t frame_index) {
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        if (it->second.seen) {
            ++it;
        } else {
            // Its voxels are recomposed this frame, so none names the slot
            // by the time an object that takes it next is composed.
            add_dirty_region(it->second.bounds);
            if (it->second.slot != NO_SDF_SLOT) free_slots_.push_back(it->second.slot);
            it = tracked_.erase(it);
        }
    }

    if (!slots_.empty()) {
        std::memcpy(device_->mapped(slot_tables_[frame_index]).data(), slots_.data(),
                    slots_.size() * sizeof(GpuSdfSlot));
    }

    // Past this many regions, or this share of the volume, one dispatch
    // over everything is cheaper than the bookkeeping.
    constexpr size_t MAX_REGIONS = 32;
    if (dirty_regions_.size() > MAX_REGIONS) {
        full_rebuild_ = true;
    } else if (!full_rebuild_) {
        glm::vec3 v = voxel_size();
        double region_voxels = 0.0;
        for (const auto& r : dirty_regions_) {
            glm::vec3 extent = glm::max(r.max - r.min, glm::vec3(0.0f)) / v;
            region_voxels += double(extent.x) * double(extent.y) * double(extent.z);
        }
        double total = double(resolution_) * resolution_ * resolution_;
        if (region_voxels > total * 0.5) full_rebuild_ = true;
    }
    if (full_rebuild_) dirty_regions_.clear();
}

void SceneSdfPass::add_dirty_region(AABB region) {
    if (!region.valid()) return;
    region.min -= glm::vec3(max_distance());
    region.max += glm::vec3(max_distance());

    // Merge with everything it touches; a merge can bridge two others, so
    // repeat until nothing overlaps.
    bool merged = true;
    while (merged) {
        merged = false;
        for (auto it = dirty_regions_.begin(); it != dirty_regions_.end(); ++it) {
            if (!it->intersects(region)) continue;
            region.expand(it->min);
            region.expand(it->max);
            dirty_regions_.erase(it);
            merged = true;
            break;
        }
    }
    dirty_regions_.push_back(region);
}

void SceneSdfPass::compose(gpu::CommandList& cmd, uint32_t frame_index) {
    const std::span<std::byte> instance_memory = device_->mapped(instance_buffers_[frame_index]);

    // Each region gets the instances whose distance band reaches into it,
    // laid out contiguously so one push constant range addresses them.
    struct Dispatch {
        glm::ivec3 voxel_min;
        glm::ivec3 voxel_extent;
        uint32_t instance_offset;
        uint32_t instance_count;
    };
    std::vector<Dispatch> dispatches;
    std::vector<GpuSdfInstance> upload;
    const auto capacity = static_cast<uint32_t>(instance_memory.size() / sizeof(GpuSdfInstance));

    if (full_rebuild_) {
        upload = gathered_;
        dispatches.push_back({glm::ivec3(0), glm::ivec3(int(resolution_)), 0,
                              static_cast<uint32_t>(upload.size())});
    } else {
        glm::vec3 v = voxel_size();
        float band = max_distance();
        for (const auto& region : dirty_regions_) {
            glm::ivec3 lo = glm::ivec3(glm::floor((region.min - volume_min_) / v));
            glm::ivec3 hi = glm::ivec3(glm::ceil((region.max - volume_min_) / v));
            lo = glm::clamp(lo, glm::ivec3(0), glm::ivec3(int(resolution_)));
            hi = glm::clamp(hi, glm::ivec3(0), glm::ivec3(int(resolution_)));
            if (glm::any(glm::lessThanEqual(hi - lo, glm::ivec3(0)))) continue;

            // Voxel-aligned box back in world space decides which instances
            // can reach it.
            AABB box{volume_min_ + glm::vec3(lo) * v, volume_min_ + glm::vec3(hi) * v};
            Dispatch d{lo, hi - lo, static_cast<uint32_t>(upload.size()), 0};
            for (size_t i = 0; i < gathered_.size(); ++i) {
                AABB reach = gathered_bounds_[i];
                reach.min -= glm::vec3(band);
                reach.max += glm::vec3(band);
                if (!reach.intersects(box)) continue;
                upload.push_back(gathered_[i]);
                ++d.instance_count;
            }
            dispatches.push_back(d);
        }
    }

    if (upload.size() > capacity) {
        FJELL_GFX_WARN("SDF instance buffer too small ({} instances), rebuilding everything",
                       upload.size());
        upload = gathered_;
        if (upload.size() > capacity) upload.resize(capacity);
        dispatches.assign(1, {glm::ivec3(0), glm::ivec3(int(resolution_)), 0,
                              static_cast<uint32_t>(upload.size())});
    }
    if (!upload.empty()) {
        std::memcpy(instance_memory.data(), upload.data(), upload.size() * sizeof(GpuSdfInstance));
    }

    // The regions are apart, so their dispatches write different voxels and
    // need nothing between them.
    cmd.set_pipeline(composite_pipeline_);
    cmd.bind({{"scene_sdf", gpu::storage(volume_)},
              {"MeshSdfData", gpu::storage(mesh_sdf_buffer_)},
              {"SdfMetadata", gpu::storage(instance_buffers_[frame_index])},
              {"scene_sdf_index", gpu::storage(index_volume_)}});
    for (const auto& d : dispatches) {
        cmd.push(ComposePush{volume_min_, d.instance_offset, volume_max_, d.instance_count,
                             d.voxel_min, resolution_, d.voxel_extent, max_distance()});
        const glm::ivec3 groups = (d.voxel_extent + 3) / 4;
        cmd.dispatch(uint32_t(groups.x), uint32_t(groups.y), uint32_t(groups.z));
    }

    dirty_regions_.clear();
    full_rebuild_ = false;
    composed_ = true;
}

void SceneSdfPass::request_slice(uint32_t axis, float slice_pos, std::optional<glm::uvec2> texel) {
    if (!slice_texture_ && !make_slice()) return;
    slice_requested_ = true;
    slice_axis_ = axis;
    slice_pos_ = slice_pos;
    slice_texel_ = texel;
}

float SceneSdfPass::slice_surface() const {
    // Half a voxel: the band in which the field crosses zero.
    const glm::vec3 v = voxel_size();
    return 0.5f * std::max({v.x, v.y, v.z});
}

bool SceneSdfPass::make_slice() {
    auto pipeline = device_->create(gpu::ComputePipelineDesc{
        .shader = "shaders/sdf_slice.comp",
        .name = "sdf_slice",
    });
    auto texture = device_->create(gpu::TextureDesc{
        .format = gpu::Format::rgba8_unorm,
        .width = SLICE_SIZE,
        .height = SLICE_SIZE,
        .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
        .initial = gpu::Clear{0.0f, 0.0f, 0.0f, 1.0f},
        .name = "sdf slice",
    });
    // Every frame in flight's slice of distances, read on the CPU once that
    // frame is done.
    auto distances = device_->create(gpu::BufferDesc{
        .size = uint64_t{sizeof(float)} * SLICE_SIZE * SLICE_SIZE * MAX_FRAMES_IN_FLIGHT,
        .use = gpu::BufferUse::storage,
        .memory = gpu::Memory::readback,
        .name = "sdf slice distances",
    });
    if (!pipeline || !texture || !distances) {
        FJELL_GFX_ERROR("The SDF slice cannot be drawn: {}",
                        !pipeline ? pipeline.error() : !texture ? texture.error() : distances.error());
        return false;
    }
    slice_pipeline_ = std::move(*pipeline);
    slice_texture_ = std::move(*texture);
    slice_distances_ = std::move(*distances);
    slice_written_.fill(false);
    slice_sampler_ = device_->sampler({.filter = gpu::Filter::nearest,
                                       .mip_filter = gpu::Filter::nearest,
                                       .address = gpu::Address::clamp});
    return true;
}

void SceneSdfPass::draw_slice(gpu::CommandList& cmd, uint32_t frame_index) {
    // This frame in flight's slice was last written MAX_FRAMES_IN_FLIGHT
    // frames ago and that frame is done: read the hovered texel's distance
    // from it before this frame writes it again.
    const uint64_t slice_floats = uint64_t{SLICE_SIZE} * SLICE_SIZE;
    const uint64_t offset = slice_floats * frame_index;
    hovered_distance_.reset();
    if (slice_texel_ && slice_written_[frame_index] && slice_texel_->x < SLICE_SIZE &&
        slice_texel_->y < SLICE_SIZE) {
        const uint64_t at = offset + uint64_t{slice_texel_->y} * SLICE_SIZE + slice_texel_->x;
        float distance = 0.0f;
        std::memcpy(&distance, device_->mapped(slice_distances_).data() + at * sizeof(float), sizeof(float));
        hovered_distance_ = distance;
    }
    slice_written_[frame_index] = true;

    cmd.set_pipeline(slice_pipeline_);
    cmd.bind({{"scene_sdf", gpu::sampled(volume_, volume_sampler_)},
              {"slice_output", gpu::storage(slice_texture_)},
              {"SliceDistances", gpu::storage(slice_distances_)}});
    cmd.push(SlicePush{slice_axis_, slice_pos_, SLICE_SIZE, max_distance(), slice_surface(),
                       static_cast<uint32_t>(offset)});
    const uint32_t groups = (SLICE_SIZE + 7) / 8;
    cmd.dispatch(groups, groups, 1);

    // The distances, for the CPU once the frame is done.
    cmd.barrier(gpu::BufferRange(slice_distances_, offset * sizeof(float), slice_floats * sizeof(float)),
                gpu::Access::storage_buffer_write_compute, gpu::Access::host_read);
}

} // namespace fjell
