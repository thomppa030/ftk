#include "renderer/frame_graph.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "renderer/pass_builder.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <latch>
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

ImageUsage image_usage_for(ResourceAccess a) {
    switch (a) {
        case ResourceAccess::color_attachment:
            return ImageUsage::color_attachment;
        case ResourceAccess::depth_attachment:
            return ImageUsage::depth_attachment;
        case ResourceAccess::depth_attachment_read:
        case ResourceAccess::input_attachment:
            return ImageUsage::depth_attachment_read;
        case ResourceAccess::sampled_fragment:
        case ResourceAccess::sampled_vertex:
            return ImageUsage::shader_read;
        case ResourceAccess::sampled_compute:
            return ImageUsage::compute_read;
        // A storage image is only ever read in GENERAL, whatever the access:
        // imageLoad through a SHADER_READ_ONLY_OPTIMAL layout is invalid.
        case ResourceAccess::storage_read_compute:
            return ImageUsage::compute_storage_read;
        case ResourceAccess::storage_write_compute:
            return ImageUsage::compute_write;
        case ResourceAccess::storage_read_write_compute:
            return ImageUsage::compute_read_write;
        case ResourceAccess::depth_read_sampled:
            return ImageUsage::depth_read_sampled;
        case ResourceAccess::sampled_raytracing:
            return ImageUsage::raytracing_read;
        case ResourceAccess::storage_write_raytracing:
            return ImageUsage::raytracing_write;
        case ResourceAccess::transfer_src:
            return ImageUsage::transfer_src;
        case ResourceAccess::transfer_dst:
            return ImageUsage::transfer_dst;
        default:
            return ImageUsage::shader_read;
    }
}

[[nodiscard]] bool access_applies_to_image(ResourceAccess a) {
    switch (a) {
        case ResourceAccess::color_attachment:
        case ResourceAccess::depth_attachment:
        case ResourceAccess::depth_attachment_read:
        case ResourceAccess::depth_read_sampled:
        case ResourceAccess::input_attachment:
        case ResourceAccess::sampled_fragment:
        case ResourceAccess::sampled_vertex:
        case ResourceAccess::sampled_compute:
        case ResourceAccess::storage_read_compute:
        case ResourceAccess::storage_write_compute:
        case ResourceAccess::storage_read_write_compute:
        case ResourceAccess::sampled_raytracing:
        case ResourceAccess::storage_write_raytracing:
        case ResourceAccess::transfer_src:
        case ResourceAccess::transfer_dst:
            return true;
        default:
            return false;
    }
}

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

uint32_t FrameGraph::register_image(VkImage image, VkImageAspectFlags aspect,
                                     uint32_t base_layer, uint32_t layer_count,
                                     uint32_t mip_count,
                                     bool persistent,
                                     VkImageLayout initial_layout) {
    const ImageKey key{image, base_layer, layer_count};
    if (auto it = image_index_.find(key); it != image_index_.end()) {
        auto& existing = images_[it->second];
        existing.persistent = existing.persistent || persistent;
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

    uint32_t id = static_cast<uint32_t>(images_.size());
    TrackedImage img{};
    img.image = image;
    img.aspect = aspect;
    img.mip_count = mip_count;
    img.array_layers = layer_count;
    img.base_layer = base_layer;
    img.persistent = persistent;

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
        slice.layout = initial_layout;
        img.slices.push_back(slice);
    }
    if (layout_trace_enabled() && image != VK_NULL_HANDLE) {
        FJELL_GFX_INFO("[layout] register img=0x{:x} layers={}+{} mips={} start={}{}",
                       reinterpret_cast<uintptr_t>(image), base_layer, layer_count,
                       mip_count, layout_str(img.slices.front().layout),
                       seeded ? " (remembered)" : "");
    }

    images_.push_back(std::move(img));
    image_index_.emplace(key, id);
    return id;
}

void FrameGraph::new_frame() {
    ++frame_serial_;
    // An image no run imported last frame is gone or idle; either way its
    // state is not worth carrying, and a handle the driver reuses must not
    // inherit it.
    for (auto it = remembered_.begin(); it != remembered_.end();) {
        if (it->second.seen + 1 < frame_serial_) {
            it = remembered_.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameGraph::begin_frame() {
    passes_.clear();
    images_.clear();
    image_index_.clear();
    virtual_index_.clear();
}

void FrameGraph::remember_states() {
    for (const auto& img : images_) {
        if (img.virtual_resource || img.image == VK_NULL_HANDLE) { continue; }
        auto& state = remembered_[ImageKey{img.image, img.base_layer, img.array_layers}];
        state.slices.assign(img.slices.begin(), img.slices.end());
        state.seen = frame_serial_;
    }
}

void FrameGraph::bind_virtual_image(uint32_t image_id, VkImage image) {
    if (image_id >= images_.size()) { return; }
    auto& img = images_[image_id];
    if (!img.virtual_resource) { return; }
    img.image = image;
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
            imp.image, imp.aspect, imp.base_layer, imp.layer_count, imp.mip_count,
            imp.persistent, imp.initial_layout);
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

    PassDecl pass;
    pass.name = name;
    pass.execute = std::move(execute);
    pass.parallel_group = builder.parallel_group();
    pass.queue = builder.queue();
    pass.image_uses.reserve(builder.texture_accesses().size());

    for (const auto& acc : builder.texture_accesses()) {
        if (!access_applies_to_image(acc.access)) { continue; }
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
        out.usage = image_usage_for(acc.access);
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

    pass.final_layouts.reserve(builder.final_layouts().size());
    for (const auto& fl : builder.final_layouts()) {
        if (fl.handle.id >= handle_to_image_id.size()) { continue; }
        uint32_t image_id = handle_to_image_id[fl.handle.id];
        if (image_id == UINT32_MAX) { continue; }

        FinalLayoutOverride out{};
        out.image_id = image_id;
        out.layout = fl.layout;
        out.range.aspect = images_[image_id].aspect;
        out.range.base_mip = 0;
        out.range.mip_count = images_[image_id].mip_count;
        out.range.base_layer = images_[image_id].base_layer;
        out.range.layer_count = images_[image_id].array_layers;
        out.last_stage = fl.last_stage;
        out.last_access = fl.last_access;
        pass.final_layouts.push_back(out);
    }

    passes_.push_back(std::move(pass));
}

namespace {

VkImageUsageFlags usage_flag_for(ImageUsage u) {
    switch (u) {
        case ImageUsage::color_attachment:       return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        case ImageUsage::depth_attachment:
        case ImageUsage::depth_attachment_read:  return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        case ImageUsage::shader_read:
        case ImageUsage::compute_read:
        case ImageUsage::raytracing_read:        return VK_IMAGE_USAGE_SAMPLED_BIT;
        case ImageUsage::compute_storage_read:
        case ImageUsage::compute_write:
        case ImageUsage::compute_read_write:
        case ImageUsage::raytracing_write:       return VK_IMAGE_USAGE_STORAGE_BIT;
        case ImageUsage::depth_read_sampled:     return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                      | VK_IMAGE_USAGE_SAMPLED_BIT;
        case ImageUsage::transfer_src:           return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        case ImageUsage::transfer_dst:           return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return 0;
}

} // namespace

void FrameGraph::compute_lifetimes(std::vector<ResourceLifetime>& out) const {
    out.assign(images_.size(), ResourceLifetime{});
    for (uint32_t p = 0; p < passes_.size(); ++p) {
        for (const auto& acc : passes_[p].image_uses) {
            auto& lt = out[acc.image_id];
            if (p < lt.first_pass) { lt.first_pass = p; }
            if (p > lt.last_pass || !lt.used()) { lt.last_pass = p; }
            lt.usage_flags |= usage_flag_for(acc.usage);
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
        FJELL_GFX_INFO("  img#{} [{}] [{}..{}] ({} passes) usage=0x{:x} first='{}' last='{}'",
                       static_cast<unsigned>(i), kind,
                       lt.first_pass, lt.last_pass,
                       lt.last_pass - lt.first_pass + 1,
                       static_cast<unsigned>(lt.usage_flags),
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
        && a.view_type == b.view_type;
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

VkImageLayout FrameGraph::layout_for(ImageUsage usage, VkImageAspectFlags aspect) {
    switch (usage) {
        case ImageUsage::color_attachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageUsage::depth_attachment:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case ImageUsage::depth_attachment_read:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case ImageUsage::shader_read:
        case ImageUsage::compute_read:
        case ImageUsage::raytracing_read:
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageUsage::compute_storage_read:
        case ImageUsage::compute_write:
        case ImageUsage::compute_read_write:
        case ImageUsage::raytracing_write:
            return VK_IMAGE_LAYOUT_GENERAL;
        case ImageUsage::depth_read_sampled:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ImageUsage::transfer_src:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ImageUsage::transfer_dst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkPipelineStageFlags2 FrameGraph::stage_for(ImageUsage usage) {
    switch (usage) {
        case ImageUsage::color_attachment:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case ImageUsage::depth_attachment:
        case ImageUsage::depth_attachment_read:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case ImageUsage::shader_read:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case ImageUsage::compute_read:
        case ImageUsage::compute_storage_read:
        case ImageUsage::compute_write:
        case ImageUsage::compute_read_write:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case ImageUsage::depth_read_sampled:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case ImageUsage::raytracing_read:
        case ImageUsage::raytracing_write:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case ImageUsage::transfer_src:
        case ImageUsage::transfer_dst:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

VkPipelineStageFlags2 FrameGraph::stages_for_queue(VkPipelineStageFlags2 stages,
                                                    QueueType queue) {
    if (queue != QueueType::async_compute) { return stages; }

    constexpr VkPipelineStageFlags2 graphics_only =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
      | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
      | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
      | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
      | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
      | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT
      | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT
      | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT
      | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
      | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT
      | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;

    const VkPipelineStageFlags2 disallowed = stages & graphics_only;
    if (disallowed == 0) { return stages; }

    // Collapse the graphics-only bits to ALL_COMMANDS; keep the rest.
    // Semaphore ordering from submit_and_present handles the real cross-
    // queue wait, so we only need a stage the queue accepts.
    return (stages & ~graphics_only) | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

VkAccessFlags2 FrameGraph::access_for(ImageUsage usage) {
    switch (usage) {
        case ImageUsage::color_attachment:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case ImageUsage::depth_attachment:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case ImageUsage::depth_attachment_read:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case ImageUsage::shader_read:
        case ImageUsage::compute_read:
        case ImageUsage::raytracing_read:
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case ImageUsage::compute_storage_read:
            return VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        case ImageUsage::compute_write:
        case ImageUsage::raytracing_write:
            return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case ImageUsage::compute_read_write:
            return VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case ImageUsage::depth_read_sampled:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case ImageUsage::transfer_src:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case ImageUsage::transfer_dst:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    return 0;
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
                if (a.layout != b.layout || a.last_stage != b.last_stage
                    || a.last_access != b.last_access) { continue; }
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
                                          ImageSlice& slice,
                                          VkImageLayout new_layout,
                                          VkPipelineStageFlags2 dst_stage,
                                          VkAccessFlags2 dst_access,
                                          QueueType queue) {
    if (slice.layout == new_layout
        && (slice.last_access & dst_access) == dst_access) {
        // Another read of a slice already visible to this access needs
        // no barrier, but the next write to it must wait for this reader
        // too: fold its stage into the state so that barrier's srcStage
        // covers every reader since the last write.
        slice.last_stage |= stages_for_queue(dst_stage, queue);
        return;
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    // Stage masks have to be legal for the queue we're recording into.
    // The semaphore wait in submit_and_present already handles any cross-
    // queue ordering, so collapsing graphics-only bits on the compute CB
    // loses no information.
    //
    // When the previous access used graphics-only stages and this barrier
    // is recorded on the compute queue, the timeline-semaphore wait in
    // submit_and_present is what actually synchronises against that work.
    // The source side of this barrier reduces to a no-op execution
    // dependency: keep the layout transition, but clear srcAccess and use
    // a queue-legal srcStage so the (stage, access) pair stays valid.
    // Otherwise srcAccess like DEPTH_STENCIL_ATTACHMENT_WRITE wouldn't
    // satisfy any stage the compute queue accepts.
    const VkPipelineStageFlags2 translated_src = stages_for_queue(slice.last_stage, queue);
    const bool cross_queue = translated_src != slice.last_stage;
    barrier.srcStageMask = cross_queue ? VK_PIPELINE_STAGE_2_NONE : translated_src;
    barrier.srcAccessMask = cross_queue ? 0 : slice.last_access;
    barrier.dstStageMask = stages_for_queue(dst_stage, queue);
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = slice.layout;
    barrier.newLayout = new_layout;
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
                       layout_str(slice.layout), layout_str(new_layout),
                       static_cast<uint64_t>(barrier.srcStageMask),
                       static_cast<uint64_t>(barrier.dstStageMask),
                       queue == QueueType::async_compute ? "compute" : "graphics",
                       reinterpret_cast<uintptr_t>(cmd));
    }

    barriers_scratch_.push_back(barrier);

    // Record the queue-translated stage back into the slice. The next
    // pass's barrier will feed this as srcStage — translating once at
    // emit time means the slice state always reflects what's actually
    // valid on whichever queue last touched it.
    slice.layout = new_layout;
    slice.last_stage = stages_for_queue(dst_stage, queue);
    slice.last_access = dst_access;
}

namespace {

// Pick the layout that satisfies every declared access. If any write is
// involved we use a general/attachment write layout; pure reads keep the
// read-only layout. Mixing read + storage-write collapses to GENERAL.
VkImageLayout merge_layouts(VkImageLayout a, VkImageLayout b) {
    if (a == VK_IMAGE_LAYOUT_UNDEFINED) { return b; }
    if (b == VK_IMAGE_LAYOUT_UNDEFINED) { return a; }
    if (a == b) { return a; }

    auto is_attachment = [](VkImageLayout l) {
        return l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            || l == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
            || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    };

    // Attachment layouts dominate sampled reads within the same pass —
    // the attachment IS the access, and READ_ONLY variants satisfy
    // sampled reads too.
    if (is_attachment(a)) { return a; }
    if (is_attachment(b)) { return b; }

    // Anything else that doesn't agree: fall back to GENERAL. Covers
    // STORAGE_WRITE + SHADER_READ, STORAGE_WRITE + TRANSFER_DST, etc.
    return VK_IMAGE_LAYOUT_GENERAL;
}

} // namespace

void FrameGraph::emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass) {
    // Merge every declared access per image into one required pre-pass
    // state. Iteration order of image_uses doesn't matter for the merge —
    // we union stages + access flags and collapse layouts.
    auto& merged = merged_scratch_;
    merged.clear();
    barriers_scratch_.clear();

    for (const auto& acc : pass.image_uses) {
        auto& img = images_[acc.image_id];
        // Virtual resources are backed by a TransientImagePool allocation
        // (bound via bind_virtual_image) after compute_alias_groups runs.
        // If the pool didn't hand out a VkImage this frame, there's nothing
        // to barrier.
        if (img.image == VK_NULL_HANDLE) { continue; }

        auto layout = layout_for(acc.usage, img.aspect);
        auto stages = stage_for(acc.usage);
        auto access = access_for(acc.usage);
        bool is_write = acc.usage == ImageUsage::color_attachment
                     || acc.usage == ImageUsage::depth_attachment
                     || acc.usage == ImageUsage::compute_write
                     || acc.usage == ImageUsage::compute_read_write
                     || acc.usage == ImageUsage::raytracing_write
                     || acc.usage == ImageUsage::transfer_dst;

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
        m->layout = merge_layouts(m->layout, layout);
        m->stages |= stages;
        m->access |= access;
        m->any_write = m->any_write || is_write;
    }

    for (const auto& m : merged) {
        auto& img = images_[m.image_id];
        carve_slices(img, m.range, indices_scratch_);
        for (size_t idx : indices_scratch_) {
            append_barrier_for_slice(cmd, img, img.slices[idx],
                                     m.layout, m.stages, m.access,
                                     pass.queue);
        }
        coalesce_slices(img);
    }

    if (barriers_scratch_.empty()) { return; }
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers_scratch_.size());
    dep.pImageMemoryBarriers = barriers_scratch_.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

void FrameGraph::apply_final_layouts(const PassDecl& pass) {
    const bool trace = layout_trace_enabled();
    for (const auto& fl : pass.final_layouts) {
        auto& img = images_[fl.image_id];
        if (img.image == VK_NULL_HANDLE) { continue; }
        if (trace) {
            FJELL_GFX_INFO("[layout] final_layout pass='{}' img=0x{:x} -> {}",
                           pass.name, reinterpret_cast<uintptr_t>(img.image),
                           layout_str(fl.layout));
        }
        carve_slices(img, fl.range, indices_scratch_);
        for (size_t idx : indices_scratch_) {
            img.slices[idx].layout = fl.layout;
            img.slices[idx].last_stage = fl.last_stage;
            img.slices[idx].last_access = fl.last_access;
        }
        coalesce_slices(img);
    }
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
                apply_final_layouts(pass);
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
        std::latch done(static_cast<ptrdiff_t>(group_size));

        for (size_t p = 0; p < group_size; ++p) {
            auto thread_idx = static_cast<uint32_t>(p % (pool->thread_count() + 1));
            VkCommandBuffer secondary = cmd_pools->allocate_secondary(thread_idx, frame_index);
            secondaries[p] = secondary;

            auto record = [&passes = passes_, group_begin, p, secondary, &done]() {
                VkCommandBufferInheritanceInfo inheritance{};
                inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

                VkCommandBufferBeginInfo begin_info{};
                begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                begin_info.pInheritanceInfo = &inheritance;

                vkBeginCommandBuffer(secondary, &begin_info);
                passes[group_begin + p].execute(secondary);
                vkEndCommandBuffer(secondary);
                done.count_down();
            };

            if (p < group_size - 1) {
                (void)pool->submit(std::move(record));
            } else {
                record();
            }
        }

        done.wait();

        // Parallel groups are graphics-only (depth_prepass + shadow are
        // the only current users). Pick the first pass's CB; a mixed-
        // queue parallel group isn't supported — we'd need separate
        // execute_commands into each CB.
        vkCmdExecuteCommands(cb_for(passes_[group_begin], group_begin),
                              static_cast<uint32_t>(group_size), secondaries.data());

        for (size_t p = group_begin; p < group_begin + group_size; ++p) {
            apply_final_layouts(passes_[p]);
        }
    }
    remember_states();
    return recorded_async;
}

} // namespace fjell
