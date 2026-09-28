#include "renderer/frame_graph.hpp"
#include "gpu/vulkan/access.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/pass_builder.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <format>
#include <fstream>
#include <unordered_map>

namespace fjell {

namespace {

// Targeted tracing for the GTAO-async layout race (Step 4). Set
// FJELL_TRACE_LAYOUT=1 to dump every begin_frame reset and every
// graph-emitted image barrier with handle, name, layouts, queue.
[[nodiscard]] bool layout_trace_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("FJELL_TRACE_LAYOUT");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

// A layout by its short name, or its number for one the trace does not name.
[[nodiscard]] std::string layout_name(VkImageLayout l);

// The barrier trace. FJELL_LOG_BARRIERS=<file> writes every barrier the
// graph emits during a window of frames to <file>: FJELL_LOG_BARRIERS_FRAME
// frames in (300 by default), for three frames. Resources go by the names
// they were declared under and masks as numbers, so the traces of two runs
// or two builds compare line for line.
struct BarrierTrace {
    std::ofstream file;
    uint64_t first_frame{300};
    uint64_t frames{3};
};

[[nodiscard]] BarrierTrace* barrier_trace() {
    static BarrierTrace* const trace = []() -> BarrierTrace* {
        const char* path = std::getenv("FJELL_LOG_BARRIERS");
        if (path == nullptr || path[0] == '\0') { return nullptr; }
        static BarrierTrace opened;
        opened.file.open(path);
        if (const char* first = std::getenv("FJELL_LOG_BARRIERS_FRAME")) {
            opened.first_frame = std::strtoull(first, nullptr, 10);
        }
        return &opened;
    }();
    return trace;
}

[[nodiscard]] const char* layout_str(VkImageLayout l) {
    switch (l) {
        case VK_IMAGE_LAYOUT_UNDEFINED:                       return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:                         return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:        return "COLOR_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:return "DS_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DS_RO";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:        return "SHADER_RO";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:            return "XFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:            return "XFER_DST";
        case VK_IMAGE_LAYOUT_PREINITIALIZED:                  return "PREINIT";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:        return "DEPTH_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:         return "DEPTH_RO";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:                 return "PRESENT";
        default:                                              return "?";
    }
}

std::string layout_name(VkImageLayout l) {
    const std::string name = layout_str(l);
    return name == "?" ? std::to_string(static_cast<int>(l)) : name;
}

// The access bits that write memory, as opposed to reading it.
constexpr VkAccessFlags2 WRITE_ACCESS_BITS =
    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
  | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT
  | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
  | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

// Clamp an externally-specified range against the image's actual
// dimensions. VK_REMAINING_* expands to "whole image from base_*".
SubresourceRange clamp_range(const TrackedImage& img, SubresourceRange r) {
    if (r.aspect == 0) { r.aspect = img.aspect; }
    if (r.mip_count == VK_REMAINING_MIP_LEVELS || r.base_mip + r.mip_count > img.mip_count) {
        r.mip_count = (r.base_mip < img.mip_count) ? img.mip_count - r.base_mip : 0;
    }
    if (r.layer_count == VK_REMAINING_ARRAY_LAYERS || r.base_layer + r.layer_count > img.array_layers) {
        r.layer_count = (r.base_layer < img.array_layers) ? img.array_layers - r.base_layer : 0;
    }
    return r;
}

bool ranges_intersect(const SubresourceRange& a, const SubresourceRange& b) {
    auto a_mip_end = a.base_mip + a.mip_count;
    auto b_mip_end = b.base_mip + b.mip_count;
    auto a_lay_end = a.base_layer + a.layer_count;
    auto b_lay_end = b.base_layer + b.layer_count;
    return a.base_mip < b_mip_end && b.base_mip < a_mip_end
        && a.base_layer < b_lay_end && b.base_layer < a_lay_end;
}

bool range_contains(const SubresourceRange& outer, const SubresourceRange& inner) {
    return outer.base_mip <= inner.base_mip
        && outer.base_mip + outer.mip_count >= inner.base_mip + inner.mip_count
        && outer.base_layer <= inner.base_layer
        && outer.base_layer + outer.layer_count >= inner.base_layer + inner.layer_count;
}

} // namespace

uint32_t FrameGraph::register_image(const gpu::TextureView& view, bool persistent,
                                     std::string_view name, gpu::AccessSet resting,
                                     bool unwritten) {
    const gpu::ResolvedView tracked = gpu::resolve(view, device_->info(view.texture));
    const uint32_t base_layer = tracked.base_layer;
    const uint32_t layer_count = tracked.layer_count;
    const uint32_t mip_count = tracked.base_mip + tracked.mip_count;
    const ImageKey key{view.texture, base_layer, layer_count};
    if (auto it = image_index_.find(key); it != image_index_.end()) {
        auto& existing = images_[it->second];
        existing.persistent = existing.persistent || persistent;
        if (existing.resting.empty()) { existing.resting = resting; }
        if (tracing_ && existing.name.empty()) { existing.name = name; }
        if (mip_count > existing.mip_count) {
            // An earlier importer undersized the mip count; widen the
            // tracked range to what the image really has.
            existing.mip_count = mip_count;
            if (existing.slices.size() == 1) {
                existing.slices.front().range.mip_count = mip_count;
            }
        }
        return it->second;
    }

    const VkImage image = gpu::vulkan::native_image(*device_, view.texture);
    const VkImageAspectFlags aspect = gpu::vulkan::native_aspect(*device_, view.texture);
    uint32_t id = static_cast<uint32_t>(images_.size());
    TrackedImage img{};
    img.texture = view.texture;
    img.image = image;
    img.aspect = aspect;
    img.mip_count = mip_count;
    img.array_layers = layer_count;
    img.base_layer = base_layer;
    img.persistent = persistent;
    img.resting = resting;
    // Without a memory of it, it starts at rest once something has written
    // it; else nothing it holds is known.
    const bool depth = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    gpu::AccessSet start{};
    if (!resting.empty()) {
        if (gpu::vulkan::image_scope(resting, depth).has_value()) {
            if (!unwritten) { start = resting; }
        } else {
            FJELL_GFX_WARN("FrameGraph: '{}' rests in accesses that need different layouts",
                           std::string(name));
            img.resting = {};
        }
    }

    // Start from where the last run left the image. A remembered state
    // whose slices no longer describe this range (the image was widened
    // to more mips since) is not trusted; the declared layout stands.
    const auto remembered = remembered_.find(key);
    bool seeded = false;
    if (remembered != remembered_.end()) {
        uint32_t covered = 0;
        for (const auto& slice : remembered->second.slices) {
            covered += slice.range.mip_count * slice.range.layer_count;
        }
        if (covered == mip_count * layer_count) {
            img.slices.assign(remembered->second.slices.begin(),
                              remembered->second.slices.end());
            seeded = true;
        }
    }
    if (!seeded) {
        ImageSlice slice{};
        slice.range.aspect = aspect;
        slice.range.base_mip = 0;
        slice.range.mip_count = mip_count;
        slice.range.base_layer = base_layer;
        slice.range.layer_count = layer_count;
        slice.in = start;
        img.slices.push_back(slice);
    }
    if (tracing_) { img.name = name; }
    if (layout_trace_enabled() && image != VK_NULL_HANDLE) {
        FJELL_GFX_INFO("[layout] register img=0x{:x} '{}' layers={}+{} mips={} start={}{}",
                       reinterpret_cast<uintptr_t>(image), name, base_layer, layer_count,
                       mip_count,
                       layout_str(gpu::vulkan::merged_image_scope(img.slices.front().in, depth).layout),
                       seeded ? " (remembered)" : "");
    }

    images_.push_back(std::move(img));
    image_index_.emplace(key, id);
    return id;
}

uint32_t FrameGraph::register_buffer(gpu::Buffer buffer, bool persistent, std::string_view name) {
    if (auto it = buffer_index_.find(buffer); it != buffer_index_.end()) {
        buffers_[it->second].persistent = buffers_[it->second].persistent || persistent;
        return it->second;
    }
    const auto id = static_cast<uint32_t>(buffers_.size());
    TrackedBuffer buf{};
    buf.shared = buffer;
    buf.buffer = gpu::vulkan::native_buffer(*device_, buffer);
    buf.persistent = persistent;
    if (tracing_) { buf.name = name; }
    if (auto remembered = remembered_buffers_.find(buffer);
        remembered != remembered_buffers_.end()) {
        buf.state = remembered->second.state;
    }
    buffers_.push_back(buf);
    buffer_index_.emplace(buffer, id);
    return id;
}

void FrameGraph::new_frame() {
    ++frame_serial_;
    run_in_frame_ = 0;
    // An image or buffer no run imported last frame is gone or idle; either
    // way its state is not worth carrying, and a handle the driver reuses
    // must not inherit it.
    std::erase_if(remembered_, [&](const auto& entry) {
        return entry.second.seen + 1 < frame_serial_;
    });
    std::erase_if(remembered_buffers_, [&](const auto& entry) {
        return entry.second.seen + 1 < frame_serial_;
    });
}

void FrameGraph::begin_frame(gpu::Device& device) {
    device_ = &device;
    const BarrierTrace* trace = barrier_trace();
    tracing_ = trace != nullptr && frame_serial_ >= trace->first_frame &&
               frame_serial_ < trace->first_frame + trace->frames;
    if (tracing_) {
        barrier_trace()->file << "frame " << frame_serial_ - trace->first_frame << " run "
                              << run_in_frame_ << '\n';
    }
    ++run_in_frame_;
    passes_.clear();
    images_.clear();
    image_index_.clear();
    virtual_index_.clear();
    buffers_.clear();
    buffer_index_.clear();
}

void FrameGraph::remember_states() {
    for (const auto& img : images_) {
        if (img.virtual_resource || !img.texture.valid()) { continue; }
        auto& state = remembered_[ImageKey{img.texture, img.base_layer, img.array_layers}];
        state.slices.assign(img.slices.begin(), img.slices.end());
        state.seen = frame_serial_;
    }
    for (const auto& buf : buffers_) {
        auto& state = remembered_buffers_[buf.shared];
        state.state = buf.state;
        state.seen = frame_serial_;
    }
}

void FrameGraph::bind_virtual_image(uint32_t image_id, gpu::Texture texture) {
    if (image_id >= images_.size()) { return; }
    auto& img = images_[image_id];
    if (!img.virtual_resource) { return; }
    img.texture = texture;
    img.image = gpu::vulkan::native_image(*device_, texture);
}

void FrameGraph::submit_declared_pass(const std::string& name, const PassBuilder& builder,
                                       std::function<void(VkCommandBuffer)> execute) {
    // Phase 3 C2: accept created textures as virtual resources. They
    // participate in lifetime analysis and alias-group bin-packing but
    // do not get a VkImage yet — each pass's record() continues to use
    // its own Images storage. C3 swaps in real VMA-backed allocations
    // and rewires pass reads through graph-provided views.
    if (!builder.created_buffers().empty()) {
        FJELL_GFX_WARN("FrameGraph::submit_declared_pass: pass '{}' declares created "
                       "buffers, which aren't handled yet (buffer tracking arrives "
                       "in Phase 4).",
                       name.c_str());
    }

    // Resolve imported and created textures into graph image ids.
    // Imports are deduped across passes by (VkImage, layer range); creates
    // are always fresh virtual entries (one per declaration — no dedup
    // yet since we don't have a stable name registry for cross-pass
    // ping-pong reads, and the migrated passes declare one create per
    // pass anyway).
    std::vector<uint32_t> handle_to_image_id;
    if (!builder.imported_textures().empty() || !builder.created_textures().empty()) {
        uint32_t max_handle_id = 0;
        for (const auto& imp : builder.imported_textures()) {
            max_handle_id = std::max(max_handle_id, imp.handle.id);
        }
        for (const auto& cre : builder.created_textures()) {
            max_handle_id = std::max(max_handle_id, cre.handle.id);
        }
        handle_to_image_id.resize(max_handle_id + 1, UINT32_MAX);
    }
    for (const auto& imp : builder.imported_textures()) {
        handle_to_image_id[imp.handle.id] = register_image(
            imp.view, imp.persistent, imp.name, imp.resting, imp.unwritten);
    }

    // Register created textures as virtual resources. Virtual resources
    // are deduped by name across passes so a transient written by one
    // pass and read by another (both call create() with the same name +
    // desc) resolves to the same TrackedImage. VK_NULL_HANDLE image +
    // one initial slice covering the whole surface so carve_slices
    // behaves normally during any stray range queries. A transient always
    // starts at UNDEFINED: its contents are dead between runs, and the
    // pool may back two of them with one image inside a run.
    for (const auto& cre : builder.created_textures()) {
        if (auto it = virtual_index_.find(cre.name_hash); it != virtual_index_.end()) {
            handle_to_image_id[cre.handle.id] = it->second;
            continue;
        }

        TrackedImage img{};
        img.image = VK_NULL_HANDLE;
        img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        img.mip_count = cre.desc.mip_levels;
        img.array_layers = cre.desc.array_layers;
        img.base_layer = 0;
        img.persistent = cre.desc.persistent;
        img.virtual_resource = true;
        img.desc = cre.desc;
        img.name = cre.name;
        img.name_hash = cre.name_hash;
        ImageSlice slice{};
        slice.range.aspect = img.aspect;
        slice.range.base_mip = 0;
        slice.range.mip_count = img.mip_count;
        slice.range.base_layer = 0;
        slice.range.layer_count = img.array_layers;
        img.slices.push_back(slice);
        images_.push_back(std::move(img));
        uint32_t image_id = static_cast<uint32_t>(images_.size() - 1);
        virtual_index_.emplace(cre.name_hash, image_id);
        handle_to_image_id[cre.handle.id] = image_id;
    }

    // Resolve imported buffers the same way. Created buffers are not
    // allocated by the graph; they were warned about above.
    auto& handle_to_buffer_id = handle_to_buffer_scratch_;
    handle_to_buffer_id.clear();
    for (const auto& imp : builder.imported_buffers()) {
        if (imp.handle.id >= handle_to_buffer_id.size()) {
            handle_to_buffer_id.resize(imp.handle.id + 1, UINT32_MAX);
        }
        handle_to_buffer_id[imp.handle.id] = register_buffer(imp.buffer, imp.persistent, imp.name);
    }

    PassDecl pass;
    pass.name = name;
    pass.execute = std::move(execute);
    pass.parallel_group = builder.parallel_group();
    pass.queue = builder.queue();
    pass.image_uses.reserve(builder.texture_accesses().size());
    pass.buffer_uses.reserve(builder.buffer_accesses().size());
    for (const auto& acc : builder.buffer_accesses()) {
        if (acc.handle.id >= handle_to_buffer_id.size()) { continue; }
        const uint32_t buffer_id = handle_to_buffer_id[acc.handle.id];
        if (buffer_id == UINT32_MAX) { continue; }
        if (!gpu::applies_to_buffer(acc.access)) {
            FJELL_GFX_WARN("FrameGraph: pass '{}' declares a buffer access the graph has "
                           "no buffer scope for.", name.c_str());
            continue;
        }
        pass.buffer_uses.push_back(BufferUse{
            .buffer_id = buffer_id,
            .access = acc.access,
            .read = access_is_read(acc.access),
            .write = access_is_write(acc.access),
        });
    }

    for (const auto& acc : builder.texture_accesses()) {
        if (!gpu::applies_to_texture(acc.access)) { continue; }
        if (acc.handle.id >= handle_to_image_id.size()) {
            FJELL_GFX_WARN("FrameGraph::submit_declared_pass: pass '{}' accesses "
                           "texture handle {} which was not imported in this builder.",
                           name.c_str(), static_cast<unsigned>(acc.handle.id));
            continue;
        }
        uint32_t image_id = handle_to_image_id[acc.handle.id];
        if (image_id == UINT32_MAX) { continue; }

        ImageAccess out{};
        out.image_id = image_id;
        out.access = acc.access;
        // Builder-level subresource ranges aren't exposed yet; for now
        // every access covers the whole image. Per-subresource reads
        // (Hi-Z mip-level reads, per-cascade shadow reads) arrive when
        // PassBuilder::read gains a range overload.
        out.range.aspect = images_[image_id].aspect;
        out.range.base_mip = 0;
        out.range.mip_count = images_[image_id].mip_count;
        out.range.base_layer = images_[image_id].base_layer;
        out.range.layer_count = images_[image_id].array_layers;
        pass.image_uses.push_back(out);
    }

    pass.final_states.reserve(builder.final_states().size());
    for (const auto& fs : builder.final_states()) {
        if (fs.handle.id >= handle_to_image_id.size()) { continue; }
        uint32_t image_id = handle_to_image_id[fs.handle.id];
        if (image_id == UINT32_MAX) { continue; }
        FinalState out{};
        out.image_id = image_id;
        out.range.aspect = images_[image_id].aspect;
        out.range.base_mip = 0;
        out.range.mip_count = images_[image_id].mip_count;
        out.range.base_layer = images_[image_id].base_layer;
        out.range.layer_count = images_[image_id].array_layers;
        out.written_by = fs.written_by;
        out.left_as = fs.left_as;
        pass.final_states.push_back(out);
    }

    passes_.push_back(std::move(pass));
}

void FrameGraph::compute_lifetimes(std::vector<ResourceLifetime>& out) const {
    out.assign(images_.size(), ResourceLifetime{});
    for (uint32_t p = 0; p < passes_.size(); ++p) {
        for (const auto& acc : passes_[p].image_uses) {
            auto& lt = out[acc.image_id];
            if (p < lt.first_pass) { lt.first_pass = p; }
            if (p > lt.last_pass || !lt.used()) { lt.last_pass = p; }
            lt.uses |= gpu::texture_use(acc.access);
        }
    }
}

std::vector<ResourceLifetime> FrameGraph::compute_lifetimes() const {
    std::vector<ResourceLifetime> out;
    compute_lifetimes(out);
    return out;
}

void FrameGraph::log_lifetimes() const {
    const char* flag = std::getenv("FJELL_LOG_LIFETIMES");
    if (flag == nullptr || flag[0] == '0' || flag[0] == '\0') { return; }

    constexpr const char* use_names[] = {"sampled", "storage", "color_target", "depth_target"};
    const auto lifetimes = compute_lifetimes();
    FJELL_GFX_INFO("FrameGraph lifetimes ({} images, {} passes):",
                   static_cast<unsigned>(images_.size()),
                   static_cast<unsigned>(passes_.size()));
    for (size_t i = 0; i < lifetimes.size(); ++i) {
        const auto& lt = lifetimes[i];
        const char* kind = images_[i].virtual_resource
            ? "virtual"
            : (images_[i].persistent ? "persistent" : "import");
        if (!lt.used()) {
            FJELL_GFX_INFO("  img#{} [{}] unused",
                           static_cast<unsigned>(i), kind);
            continue;
        }
        std::string uses;
        lt.uses.for_each([&](gpu::TextureUse use) {
            if (!uses.empty()) { uses += '|'; }
            uses += use_names[static_cast<size_t>(use)];
        });
        FJELL_GFX_INFO("  img#{} [{}] [{}..{}] ({} passes) uses={} first='{}' last='{}'",
                       static_cast<unsigned>(i), kind,
                       lt.first_pass, lt.last_pass,
                       lt.last_pass - lt.first_pass + 1,
                       uses.empty() ? std::string("none") : uses,
                       passes_[lt.first_pass].name.c_str(),
                       passes_[lt.last_pass].name.c_str());
    }
}

namespace {

bool desc_matches_exactly(const TextureDesc& a, const TextureDesc& b) {
    return a.format == b.format
        && a.samples == b.samples
        && a.array_layers == b.array_layers
        && a.mip_levels == b.mip_levels
        && a.size_class == b.size_class
        && a.width == b.width
        && a.height == b.height
        && a.depth == b.depth
        && a.viewport_divisor == b.viewport_divisor
        && a.kind == b.kind;
}

} // namespace

std::vector<AliasGroup> FrameGraph::compute_alias_groups() const {
    std::vector<AliasGroup> groups;
    compute_alias_groups(compute_lifetimes(), groups);
    return groups;
}

void FrameGraph::compute_alias_groups(const std::vector<ResourceLifetime>& lifetimes,
                                      std::vector<AliasGroup>& groups) const {
    groups.clear();

    // Walk virtual resources in first_pass order — greedy packs need a
    // stable arrival sequence. Persistent and imported images sit out
    // of the pool entirely; C3 will allocate them separately.
    struct Candidate {
        uint32_t image_id;
        ResourceLifetime lt;
    };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < images_.size(); ++i) {
        if (!images_[i].virtual_resource) { continue; }
        if (images_[i].persistent) { continue; }
        if (!lifetimes[i].used()) { continue; }
        candidates.push_back({static_cast<uint32_t>(i), lifetimes[i]});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.lt.first_pass < b.lt.first_pass;
              });

    for (const auto& c : candidates) {
        const auto& desc = images_[c.image_id].desc;
        AliasGroup* target = nullptr;
        if (aliasing_enabled_) {
            for (auto& g : groups) {
                if (g.last_free_pass < c.lt.first_pass && desc_matches_exactly(g.desc, desc)) {
                    target = &g;
                    break;
                }
            }
        }
        if (target == nullptr) {
            groups.push_back({});
            target = &groups.back();
            target->desc = desc;
        }
        target->resource_ids.push_back(c.image_id);
        target->last_free_pass = std::max(target->last_free_pass, c.lt.last_pass);
    }
}

bool FrameGraph::validate_alias_groups(const std::vector<AliasGroup>& groups) const {
    const char* flag = std::getenv("FJELL_VALIDATE_ALIASING");
    if (flag == nullptr || flag[0] == '0' || flag[0] == '\0') { return true; }

    const auto lifetimes = compute_lifetimes();
    bool ok = true;
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& grp = groups[g];
        if (grp.resource_ids.size() < 2) { continue; }

        // Sort members by first_pass so overlap checks are a simple
        // forward sweep.
        std::vector<uint32_t> ordered = grp.resource_ids;
        std::sort(ordered.begin(), ordered.end(),
                  [&](uint32_t a, uint32_t b) {
                      return lifetimes[a].first_pass < lifetimes[b].first_pass;
                  });

        for (size_t i = 0; i + 1 < ordered.size(); ++i) {
            const auto& a = lifetimes[ordered[i]];
            const auto& b = lifetimes[ordered[i + 1]];
            if (a.last_pass >= b.first_pass) {
                FJELL_GFX_ERROR(
                    "AliasGroup#{} overlap: img#{} '{}' [{}..{}] and img#{} '{}' [{}..{}]",
                    static_cast<unsigned>(g),
                    ordered[i], images_[ordered[i]].name.c_str(),
                    a.first_pass, a.last_pass,
                    ordered[i + 1], images_[ordered[i + 1]].name.c_str(),
                    b.first_pass, b.last_pass);
                ok = false;
            }
        }
    }
    if (ok) {
        FJELL_GFX_INFO("FrameGraph aliasing validation: {} groups OK",
                       static_cast<unsigned>(groups.size()));
    }
    return ok;
}

void FrameGraph::log_alias_groups() const {
    const char* flag = std::getenv("FJELL_LOG_LIFETIMES");
    if (flag == nullptr || flag[0] == '0' || flag[0] == '\0') { return; }

    const auto groups = compute_alias_groups();
    uint32_t virtual_count = 0;
    for (const auto& img : images_) {
        if (img.virtual_resource && !img.persistent) { ++virtual_count; }
    }
    if (virtual_count == 0) {
        FJELL_GFX_INFO("FrameGraph alias groups: no virtual resources yet");
        return;
    }
    float ratio = groups.empty() ? 0.0f
        : 100.0f * (1.0f - static_cast<float>(groups.size()) / static_cast<float>(virtual_count));
    FJELL_GFX_INFO("FrameGraph alias groups: {} logical -> {} physical ({:.1f}% reduction)",
                   virtual_count,
                   static_cast<unsigned>(groups.size()),
                   ratio);
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& group = groups[g];
        FJELL_GFX_INFO("  group#{} fmt={} size_class={} w={} h={} layers={} mips={}: {} resources",
                       static_cast<unsigned>(g),
                       static_cast<int>(group.desc.format),
                       static_cast<int>(group.desc.size_class),
                       group.desc.width, group.desc.height,
                       group.desc.array_layers, group.desc.mip_levels,
                       static_cast<unsigned>(group.resource_ids.size()));
    }
}

void FrameGraph::log_queue_segments() const {
    const char* flag = std::getenv("FJELL_LOG_LIFETIMES");
    if (flag == nullptr || flag[0] == '0' || flag[0] == '\0') { return; }
    if (passes_.empty()) { return; }

    auto queue_name = [](QueueType q) {
        return q == QueueType::async_compute ? "async_compute" : "graphics";
    };

    uint32_t segment_index = 0;
    uint32_t segment_size = 0;
    uint32_t total_compute_passes = 0;
    QueueType prev_queue = passes_.front().queue;
    for (const auto& pass : passes_) {
        if (pass.queue == prev_queue) {
            ++segment_size;
        } else {
            FJELL_GFX_INFO("  segment#{} queue={} ({} pass{})",
                           segment_index, queue_name(prev_queue),
                           segment_size, segment_size == 1 ? "" : "es");
            ++segment_index;
            prev_queue = pass.queue;
            segment_size = 1;
        }
        if (pass.queue == QueueType::async_compute) { ++total_compute_passes; }
    }
    FJELL_GFX_INFO("  segment#{} queue={} ({} pass{})",
                   segment_index, queue_name(prev_queue),
                   segment_size, segment_size == 1 ? "" : "es");
    FJELL_GFX_INFO("FrameGraph queue segments: {} total, {} async compute pass(es)",
                   segment_index + 1, total_compute_passes);
}

VkPipelineStageFlags2 FrameGraph::stages_for_queue(VkPipelineStageFlags2 stages,
                                                    QueueType queue) {
    return queue == QueueType::async_compute ? gpu::vulkan::compute_queue_stages(stages) : stages;
}

void FrameGraph::carve_slices(TrackedImage& img, const SubresourceRange& query,
                              std::vector<size_t>& out) {
    // Split each overlapping slice along the query's mip/layer edges so
    // every resulting slice is either fully inside `query` or fully
    // outside. Then collect the indices of the inside ones.
    out.clear();
    SubresourceRange q = clamp_range(img, query);
    if (q.mip_count == 0 || q.layer_count == 0) { return; }
    const uint32_t q_mip_end = q.base_mip + q.mip_count;
    const uint32_t q_lay_end = q.base_layer + q.layer_count;

    auto& rebuilt = rebuilt_scratch_;
    rebuilt.clear();
    for (const auto& s : img.slices) {
        if (!ranges_intersect(s.range, q)) {
            rebuilt.push_back(s);
            continue;
        }
        // Split on mip boundaries (base_mip, q_mip_end), then on layer.
        uint32_t cuts_m[3];
        int nm = 0;
        cuts_m[nm++] = s.range.base_mip;
        if (q.base_mip > s.range.base_mip && q.base_mip < s.range.base_mip + s.range.mip_count) {
            cuts_m[nm++] = q.base_mip;
        }
        if (q_mip_end > s.range.base_mip && q_mip_end < s.range.base_mip + s.range.mip_count) {
            cuts_m[nm++] = q_mip_end;
        }
        cuts_m[nm++] = s.range.base_mip + s.range.mip_count;

        uint32_t cuts_l[3];
        int nl = 0;
        cuts_l[nl++] = s.range.base_layer;
        if (q.base_layer > s.range.base_layer && q.base_layer < s.range.base_layer + s.range.layer_count) {
            cuts_l[nl++] = q.base_layer;
        }
        if (q_lay_end > s.range.base_layer && q_lay_end < s.range.base_layer + s.range.layer_count) {
            cuts_l[nl++] = q_lay_end;
        }
        cuts_l[nl++] = s.range.base_layer + s.range.layer_count;

        for (int mi = 0; mi + 1 < nm; ++mi) {
            for (int li = 0; li + 1 < nl; ++li) {
                ImageSlice piece = s;
                piece.range.base_mip = cuts_m[mi];
                piece.range.mip_count = cuts_m[mi + 1] - cuts_m[mi];
                piece.range.base_layer = cuts_l[li];
                piece.range.layer_count = cuts_l[li + 1] - cuts_l[li];
                rebuilt.push_back(piece);
            }
        }
    }
    img.slices.swap(rebuilt);

    for (size_t i = 0; i < img.slices.size(); ++i) {
        if (range_contains(q, img.slices[i].range)) {
            out.push_back(i);
        }
    }
}

void FrameGraph::coalesce_slices(TrackedImage& img) {
    // Merge any adjacent slices whose layout/stage/access match and
    // whose ranges form a contiguous rectangle. Linear passes keep the
    // slice list small; without this it would grow with every access.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < img.slices.size() && !changed; ++i) {
            for (size_t j = i + 1; j < img.slices.size(); ++j) {
                const auto& a = img.slices[i];
                const auto& b = img.slices[j];
                if (a.in != b.in || !(a.state == b.state)) { continue; }
                if (a.range.aspect != b.range.aspect) { continue; }
                // Horizontal merge (same layer range, adjacent mips)
                if (a.range.base_layer == b.range.base_layer
                    && a.range.layer_count == b.range.layer_count
                    && a.range.base_mip + a.range.mip_count == b.range.base_mip) {
                    img.slices[i].range.mip_count += b.range.mip_count;
                    img.slices.erase(img.slices.begin() + static_cast<ptrdiff_t>(j));
                    changed = true;
                    break;
                }
                if (b.range.base_layer == a.range.base_layer
                    && b.range.layer_count == a.range.layer_count
                    && b.range.base_mip + b.range.mip_count == a.range.base_mip) {
                    img.slices[i].range.base_mip = b.range.base_mip;
                    img.slices[i].range.mip_count += b.range.mip_count;
                    img.slices.erase(img.slices.begin() + static_cast<ptrdiff_t>(j));
                    changed = true;
                    break;
                }
                // Vertical merge (same mip range, adjacent layers)
                if (a.range.base_mip == b.range.base_mip
                    && a.range.mip_count == b.range.mip_count
                    && a.range.base_layer + a.range.layer_count == b.range.base_layer) {
                    img.slices[i].range.layer_count += b.range.layer_count;
                    img.slices.erase(img.slices.begin() + static_cast<ptrdiff_t>(j));
                    changed = true;
                    break;
                }
                if (b.range.base_mip == a.range.base_mip
                    && b.range.mip_count == a.range.mip_count
                    && b.range.base_layer + b.range.layer_count == a.range.base_layer) {
                    img.slices[i].range.base_layer = b.range.base_layer;
                    img.slices[i].range.layer_count += b.range.layer_count;
                    img.slices.erase(img.slices.begin() + static_cast<ptrdiff_t>(j));
                    changed = true;
                    break;
                }
            }
        }
    }
}

void FrameGraph::append_barrier_for_slice(VkCommandBuffer cmd, const TrackedImage& img,
                                          ImageSlice& slice, gpu::AccessSet accesses,
                                          bool write, QueueType queue) {
    AccessState& s = slice.state;
    const bool depth = (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    auto scope = [&](gpu::AccessSet set) { return gpu::vulkan::merged_image_scope(set, depth); };
    const gpu::vulkan::ImageScope now = scope(slice.in);
    const gpu::vulkan::ImageScope next = scope(accesses);
    const VkPipelineStageFlags2 stage = stages_for_queue(next.stages, queue);
    const VkAccessFlags2 dst_access = next.access;
    const bool layout_change = now.layout != next.layout;

    const VkPipelineStageFlags2 write_stages = scope(s.written_by | s.changed_for).stages;
    const VkAccessFlags2 write_access = scope(s.written_by).access & WRITE_ACCESS_BITS;
    const VkPipelineStageFlags2 read_stages = scope(s.read_by).stages;
    const gpu::vulkan::ImageScope visible = scope(s.visible_to);
    const VkPipelineStageFlags2 visible_stages = stages_for_queue(visible.stages, queue);

    // The same rule as for buffers, plus the layout. A layout change or a
    // write waits for the last write and every reader since; a read waits
    // for the last write unless an earlier barrier already made it visible
    // to this stage and access.
    VkPipelineStageFlags2 src_stage = 0;
    VkAccessFlags2 src_access = 0;
    VkPipelineStageFlags2 barrier_dst = stage;
    VkAccessFlags2 barrier_dst_access = dst_access;
    gpu::AccessSet made_visible = accesses;
    bool needed = false;
    if (layout_change || write) {
        src_stage = write_stages | read_stages;
        src_access = write_access;
        needed = layout_change || src_stage != 0;
    } else if (write_stages != 0
               && ((stage & ~visible_stages) != 0 || (dst_access & ~visible.access) != 0)) {
        src_stage = write_stages;
        src_access = write_access;
        // Widen to everything made visible before, so the one scope kept
        // covers every reader so far.
        barrier_dst = stages_for_queue(next.stages | visible.stages, queue);
        barrier_dst_access = dst_access | visible.access;
        made_visible = accesses | s.visible_to;
        needed = true;
    }

    if (needed) {
        // A source stage the recording queue does not have means the last
        // access happened on the other queue, and the timeline semaphore
        // between the submits orders the work. The barrier is still needed
        // for a layout change, and its source scope has to reach the
        // semaphore wait for the transition to be ordered after it: an
        // empty one would let the transition run before the wait. Every
        // stage covers whichever stage the submit waits at; the semaphore
        // has already made the memory available, so no access is named.
        // Without a layout change the barrier would carry nothing.
        const VkPipelineStageFlags2 translated_src = stages_for_queue(src_stage, queue);
        const bool cross_queue = translated_src != src_stage;
        if (layout_change || !cross_queue) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = cross_queue      ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                 : src_stage == 0   ? VK_PIPELINE_STAGE_2_NONE
                                                    : src_stage;
            barrier.srcAccessMask = cross_queue ? 0 : src_access;
            barrier.dstStageMask = barrier_dst;
            barrier.dstAccessMask = barrier_dst_access;
            barrier.oldLayout = now.layout;
            barrier.newLayout = next.layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = img.image;
            barrier.subresourceRange.aspectMask = slice.range.aspect;
            barrier.subresourceRange.baseMipLevel = slice.range.base_mip;
            barrier.subresourceRange.levelCount = slice.range.mip_count;
            barrier.subresourceRange.baseArrayLayer = slice.range.base_layer;
            barrier.subresourceRange.layerCount = slice.range.layer_count;

            if (layout_trace_enabled() && img.image != VK_NULL_HANDLE) {
                FJELL_GFX_INFO("[layout] graph barrier img=0x{:x} {} -> {} src=0x{:x} dst=0x{:x} on {} cb=0x{:x}",
                               reinterpret_cast<uintptr_t>(img.image),
                               layout_str(now.layout), layout_str(next.layout),
                               static_cast<uint64_t>(barrier.srcStageMask),
                               static_cast<uint64_t>(barrier.dstStageMask),
                               queue == QueueType::async_compute ? "compute" : "graphics",
                               reinterpret_cast<uintptr_t>(cmd));
            }
            barriers_scratch_.push_back(barrier);
            if (tracing_) {
                trace_pending_ += std::format(
                    "    image {} mips {}+{} layers {}+{} {} -> {} src {:x}/{:x} dst {:x}/{:x}\n",
                    img.name, slice.range.base_mip, slice.range.mip_count, slice.range.base_layer,
                    slice.range.layer_count, layout_name(barrier.oldLayout),
                    layout_name(barrier.newLayout), barrier.srcStageMask, barrier.srcAccessMask,
                    barrier.dstStageMask, barrier.dstAccessMask);
            }
        }
    }

    // Record the use.
    if (write) {
        s = AccessState{.written_by = accesses};
    } else if (layout_change) {
        // The transition is ordered before this reader, so a later reader
        // chains through this reader's stage to wait for it.
        s.changed_for |= accesses;
        s.read_by = accesses;
        s.visible_to = accesses;
    } else {
        if (needed) { s.visible_to = made_visible; }
        s.read_by |= accesses;
    }
    slice.in = accesses;
}

void FrameGraph::append_barrier_for_buffer(TrackedBuffer& buf, const BufferUse& use,
                                          QueueType queue) {
    AccessState& s = buf.state;
    const gpu::vulkan::BufferScope scope = gpu::vulkan::buffer_scope(use.access);
    const VkPipelineStageFlags2 dst_stage = stages_for_queue(scope.stages, queue);
    const VkPipelineStageFlags2 write_stages = gpu::vulkan::buffer_scope(s.written_by).stages;
    const VkAccessFlags2 write_access =
        gpu::vulkan::buffer_scope(s.written_by).access & WRITE_ACCESS_BITS;
    const VkPipelineStageFlags2 read_stages = gpu::vulkan::buffer_scope(s.read_by).stages;
    const gpu::vulkan::BufferScope visible = gpu::vulkan::buffer_scope(s.visible_to);
    const VkPipelineStageFlags2 visible_stages = stages_for_queue(visible.stages, queue);

    // A write waits for the last write (write-after-write) and for every
    // reader since (write-after-read: execution only, a read leaves nothing
    // to make available). A read waits for the last write unless an earlier
    // barrier already made it visible to this stage and access.
    VkPipelineStageFlags2 src_stage = 0;
    VkAccessFlags2 src_access = 0;
    VkAccessFlags2 dst_access = scope.access;
    gpu::AccessSet made_visible = use.access;
    if (use.write) {
        src_stage = write_stages | read_stages;
        src_access = write_access;
    } else if (write_stages != 0
               && ((dst_stage & ~visible_stages) != 0 || (scope.access & ~visible.access) != 0)) {
        src_stage = write_stages;
        src_access = write_access;
        // Widen to everything made visible before, so the one scope kept
        // covers every reader so far.
        dst_access |= visible.access;
        made_visible = use.access | s.visible_to;
    }

    if (src_stage != 0) {
        const VkPipelineStageFlags2 barrier_dst =
            use.write ? dst_stage : stages_for_queue(scope.stages | visible.stages, queue);
        // Same rule as for images: a source stage the recording queue does
        // not have means the last access happened on the other queue, and
        // the timeline semaphore between the submits is what orders them.
        // An image still needs its barrier there for the layout change; a
        // buffer has none, so the barrier would carry nothing and is left
        // out.
        const VkPipelineStageFlags2 translated_src = stages_for_queue(src_stage, queue);
        const bool cross_queue = translated_src != src_stage;

        if (!cross_queue) {
            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = src_stage;
            barrier.srcAccessMask = src_access;
            barrier.dstStageMask = barrier_dst;
            barrier.dstAccessMask = dst_access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buf.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            buffer_barriers_scratch_.push_back(barrier);
            if (tracing_) {
                trace_pending_ += std::format("    buffer {} src {:x}/{:x} dst {:x}/{:x}\n",
                                              buf.name, barrier.srcStageMask,
                                              barrier.srcAccessMask, barrier.dstStageMask,
                                              barrier.dstAccessMask);
            }

            if (layout_trace_enabled()) {
                FJELL_GFX_INFO("[layout] buffer barrier buf=0x{:x} {} src=0x{:x}/0x{:x} dst=0x{:x}/0x{:x} on {}",
                               reinterpret_cast<uintptr_t>(buf.buffer),
                               use.write ? (write_stages != 0 ? "WAW/WAR" : "WAR") : "RAW",
                               static_cast<uint64_t>(barrier.srcStageMask),
                               static_cast<uint64_t>(barrier.srcAccessMask),
                               static_cast<uint64_t>(barrier.dstStageMask),
                               static_cast<uint64_t>(barrier.dstAccessMask),
                               queue == QueueType::async_compute ? "compute" : "graphics");
            }
        }

        if (!use.write) { s.visible_to = made_visible; }
    }

    if (use.write) {
        s = AccessState{.written_by = use.access};
    } else {
        s.read_by |= use.access;
    }
}

void FrameGraph::emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass) {
    if (tracing_) {
        barrier_trace()->file << "  pass " << pass.name
                              << (pass.queue == QueueType::async_compute ? " on compute\n" : "\n");
    }
    // Merge every declared access per image into one required pre-pass
    // state. Iteration order of image_uses doesn't matter for the merge —
    // we union stages + access flags and collapse layouts.
    auto& merged = merged_scratch_;
    merged.clear();
    barriers_scratch_.clear();
    buffer_barriers_scratch_.clear();

    for (const auto& acc : pass.image_uses) {
        auto& img = images_[acc.image_id];
        // Virtual resources are backed by a TransientImagePool allocation
        // (bound via bind_virtual_image) after compute_alias_groups runs.
        // If the pool didn't hand out a VkImage this frame, there's nothing
        // to barrier.
        if (img.image == VK_NULL_HANDLE) { continue; }

        const bool is_write = gpu::access_is_write(acc.access);

        // A pass touches a handful of images; a linear search beats a map.
        MergedAccess* m = nullptr;
        for (auto& entry : merged) {
            if (entry.image_id == acc.image_id) { m = &entry; break; }
        }
        if (m == nullptr) {
            // First access — range defines the covered subresource.
            merged.push_back(MergedAccess{.image_id = acc.image_id, .range = acc.range});
            m = &merged.back();
        }
        m->accesses |= acc.access;
        m->any_write = m->any_write || is_write;
    }

    for (const auto& m : merged) {
        auto& img = images_[m.image_id];
        carve_slices(img, m.range, indices_scratch_);
        for (size_t idx : indices_scratch_) {
            append_barrier_for_slice(cmd, img, img.slices[idx], m.accesses, m.any_write,
                                     pass.queue);
        }
        coalesce_slices(img);
    }

    // Buffers, merged per buffer the same way: one use carrying every
    // stage and access the pass declared on it.
    auto& merged_buffers = merged_buffers_scratch_;
    merged_buffers.clear();
    for (const auto& use : pass.buffer_uses) {
        BufferUse* m = nullptr;
        for (auto& entry : merged_buffers) {
            if (entry.buffer_id == use.buffer_id) { m = &entry; break; }
        }
        if (m == nullptr) {
            merged_buffers.push_back(use);
            continue;
        }
        m->access |= use.access;
        m->read = m->read || use.read;
        m->write = m->write || use.write;
    }
    for (const auto& use : merged_buffers) {
        append_barrier_for_buffer(buffers_[use.buffer_id], use, pass.queue);
    }

    if (tracing_) {
        barrier_trace()->file << trace_pending_;
        trace_pending_.clear();
    }
    if (barriers_scratch_.empty() && buffer_barriers_scratch_.empty()) { return; }
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers_scratch_.size());
    dep.pImageMemoryBarriers = barriers_scratch_.data();
    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers_scratch_.size());
    dep.pBufferMemoryBarriers = buffer_barriers_scratch_.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

void FrameGraph::apply_final_states(const PassDecl& pass) {
    const bool trace = layout_trace_enabled();
    for (const auto& fs : pass.final_states) {
        auto& img = images_[fs.image_id];
        if (img.image == VK_NULL_HANDLE) { continue; }
        const VkImageLayout left_layout =
            gpu::vulkan::merged_image_scope(fs.left_as, (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
                .layout;
        if (trace) {
            FJELL_GFX_INFO("[layout] leaves pass='{}' img=0x{:x} -> {}",
                           pass.name, reinterpret_cast<uintptr_t>(img.image),
                           layout_str(left_layout));
        }
        if (tracing_) {
            barrier_trace()->file << std::format("    leaves {} -> {}\n", img.name,
                                                 layout_name(left_layout));
        }
        carve_slices(img, fs.range, indices_scratch_);
        for (size_t idx : indices_scratch_) {
            // What the pass left the write visible to is where it expects
            // readers, some of which the graph never sees (ImGui showing a
            // viewport image), so the next write waits for those stages.
            img.slices[idx].in = fs.left_as;
            img.slices[idx].state = AccessState{
                .written_by = fs.written_by,
                .read_by = fs.left_as,
                .visible_to = fs.left_as,
            };
        }
        coalesce_slices(img);
    }
}

void FrameGraph::return_to_rest(VkCommandBuffer cmd, const PassDecl& pass) {
    barriers_scratch_.clear();
    for (const auto& use : pass.image_uses) {
        auto& img = images_[use.image_id];
        if (img.resting.empty() || img.image == VK_NULL_HANDLE) { continue; }
        const SubresourceRange whole{img.aspect, 0, img.mip_count, img.base_layer,
                                     img.array_layers};
        carve_slices(img, whole, indices_scratch_);
        for (size_t idx : indices_scratch_) {
            append_barrier_for_slice(cmd, img, img.slices[idx], img.resting, false, pass.queue);
        }
        coalesce_slices(img);
    }
    if (barriers_scratch_.empty()) { return; }
    if (tracing_) {
        barrier_trace()->file << "  rest after " << pass.name << '\n' << trace_pending_;
        trace_pending_.clear();
    }
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers_scratch_.size());
    dep.pImageMemoryBarriers = barriers_scratch_.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool FrameGraph::execute(VkCommandBuffer graphics_pre,
                          VkCommandBuffer graphics_post,
                          VkCommandBuffer async_compute,
                          ThreadPool* pool, ThreadCommandPools* cmd_pools,
                          uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("frame_graph_execute");
    bool can_parallelize = pool && cmd_pools && pool->thread_count() > 0;
    bool recorded_async = false;

    // Pre-scan for the first async-compute pass. Every graphics pass at
    // a lower index goes into graphics_pre, higher indices into
    // graphics_post. If no compute pass runs, graphics_post is unused
    // and everything stays in graphics_pre (which works out to today's
    // single-CB behavior). Null graphics_post also folds back into pre,
    // so devices without async support don't need a second CB.
    size_t first_async_idx = passes_.size();
    for (size_t i = 0; i < passes_.size(); ++i) {
        if (passes_[i].queue == QueueType::async_compute && async_compute != VK_NULL_HANDLE) {
            first_async_idx = i;
            break;
        }
    }
    VkCommandBuffer effective_post = (graphics_post != VK_NULL_HANDLE) ? graphics_post : graphics_pre;

    auto cb_for = [&](const PassDecl& pass, size_t index) {
        if (pass.queue == QueueType::async_compute && async_compute != VK_NULL_HANDLE) {
            recorded_async = true;
            return async_compute;
        }
        return index > first_async_idx ? effective_post : graphics_pre;
    };

    size_t i = 0;
    while (i < passes_.size()) {
        auto& pass = passes_[i];
        VkCommandBuffer cb = cb_for(pass, i);

        if (pass.parallel_group == 0 || !can_parallelize) {
            {
                FJELL_PROFILE_SCOPE_N("fg_emit_barriers");
                emit_barriers_for_pass(cb, pass);
            }
            {
                FJELL_PROFILE_SCOPE_N("fg_pass_execute");
                pass.execute(cb);
            }
            {
                FJELL_PROFILE_SCOPE_N("fg_final_layouts");
                apply_final_states(pass);
                return_to_rest(cb, pass);
            }
            ++i;
            continue;
        }

        uint32_t group = pass.parallel_group;
        size_t group_begin = i;
        while (i < passes_.size() && passes_[i].parallel_group == group) {
            ++i;
        }
        size_t group_size = i - group_begin;

        for (size_t p = group_begin; p < group_begin + group_size; ++p) {
            emit_barriers_for_pass(cb_for(passes_[p], p), passes_[p]);
        }

        secondaries_scratch_.resize(group_size);
        auto& secondaries = secondaries_scratch_;

        // A command pool may be used by one thread at a time. A chunk runs on
        // one thread, so its passes record into buffers from the pool its
        // chunk id names.
        pool->parallel_for(0, static_cast<uint32_t>(group_size), 1,
            [&](uint32_t chunk, uint32_t first, uint32_t last) {
                for (uint32_t p = first; p < last; ++p) {
                    VkCommandBuffer secondary = cmd_pools->allocate_secondary(chunk, frame_index);

                    VkCommandBufferInheritanceInfo inheritance{};
                    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

                    VkCommandBufferBeginInfo begin_info{};
                    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    begin_info.pInheritanceInfo = &inheritance;

                    vk_check(vkBeginCommandBuffer(secondary, &begin_info),
                             "Failed to begin a parallel pass's command buffer");
                    passes_[group_begin + p].execute(secondary);
                    vk_check(vkEndCommandBuffer(secondary),
                             "Failed to end a parallel pass's command buffer");
                    secondaries[p] = secondary;
                }
            });

        // Parallel groups are graphics-only (depth_prepass + shadow are
        // the only current users). Pick the first pass's CB; a mixed-
        // queue parallel group isn't supported — we'd need separate
        // execute_commands into each CB.
        vkCmdExecuteCommands(cb_for(passes_[group_begin], group_begin),
                              static_cast<uint32_t>(group_size), secondaries.data());

        for (size_t p = group_begin; p < group_begin + group_size; ++p) {
            apply_final_states(passes_[p]);
            return_to_rest(cb_for(passes_[p], p), passes_[p]);
        }
    }
    remember_states();
    if (tracing_) { barrier_trace()->file.flush(); }
    return recorded_async;
}

} // namespace fjell
