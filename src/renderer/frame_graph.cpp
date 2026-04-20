#include "renderer/frame_graph.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "renderer/pass_builder.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <cassert>
#include <cstdlib>
#include <latch>

namespace fjell {

namespace {

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
        case ResourceAccess::storage_read_compute:
            return ImageUsage::compute_read;
        case ResourceAccess::storage_write_compute:
        case ResourceAccess::storage_read_write_compute:
            return ImageUsage::compute_write;
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
        case ResourceAccess::input_attachment:
        case ResourceAccess::sampled_fragment:
        case ResourceAccess::sampled_vertex:
        case ResourceAccess::sampled_compute:
        case ResourceAccess::storage_read_compute:
        case ResourceAccess::storage_write_compute:
        case ResourceAccess::storage_read_write_compute:
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
                                     bool persistent) {
    uint32_t id = static_cast<uint32_t>(images_.size());
    TrackedImage img{};
    img.image = image;
    img.aspect = aspect;
    img.mip_count = mip_count;
    img.array_layers = layer_count;
    img.base_layer = base_layer;
    img.persistent = persistent;

    ImageSlice slice{};
    slice.range.aspect = aspect;
    slice.range.base_mip = 0;
    slice.range.mip_count = mip_count;
    slice.range.base_layer = base_layer;
    slice.range.layer_count = layer_count;
    slice.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.slices.push_back(slice);

    images_.push_back(std::move(img));
    return id;
}

void FrameGraph::begin_frame() {
    passes_.clear();
    for (auto& img : images_) {
        if (img.persistent) { continue; }
        img.slices.clear();
        ImageSlice slice{};
        slice.range.aspect = img.aspect;
        slice.range.base_mip = 0;
        slice.range.mip_count = img.mip_count;
        slice.range.base_layer = img.base_layer;
        slice.range.layer_count = img.array_layers;
        slice.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        img.slices.push_back(slice);
    }
}

void FrameGraph::add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                           std::initializer_list<std::pair<uint32_t, ImageUsage>> uses,
                           uint32_t parallel_group) {
    PassDecl pass;
    pass.name = name;
    pass.execute = std::move(execute);
    pass.image_uses.reserve(uses.size());
    for (const auto& [id, usage] : uses) {
        ImageAccess acc{};
        acc.image_id = id;
        acc.usage = usage;
        acc.range.aspect = images_[id].aspect;
        acc.range.mip_count = images_[id].mip_count;
        acc.range.base_layer = images_[id].base_layer;
        acc.range.layer_count = images_[id].array_layers;
        pass.image_uses.push_back(acc);
    }
    pass.parallel_group = parallel_group;
    passes_.push_back(std::move(pass));
}

void FrameGraph::add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                           std::vector<std::pair<uint32_t, ImageUsage>> uses,
                           uint32_t parallel_group) {
    PassDecl pass;
    pass.name = name;
    pass.execute = std::move(execute);
    pass.image_uses.reserve(uses.size());
    for (const auto& [id, usage] : uses) {
        ImageAccess acc{};
        acc.image_id = id;
        acc.usage = usage;
        acc.range.aspect = images_[id].aspect;
        acc.range.mip_count = images_[id].mip_count;
        acc.range.base_layer = images_[id].base_layer;
        acc.range.layer_count = images_[id].array_layers;
        pass.image_uses.push_back(acc);
    }
    pass.parallel_group = parallel_group;
    passes_.push_back(std::move(pass));
}

void FrameGraph::submit_declared_pass(const std::string& name, const PassBuilder& builder,
                                       std::function<void(VkCommandBuffer)> execute) {
    if (!builder.created_textures().empty() || !builder.created_buffers().empty()) {
        FJELL_GFX_WARN("FrameGraph::submit_declared_pass: pass '{}' declares created "
                       "resources, but transient allocation lands with Phase 3. "
                       "Importing is the only supported mode until then.",
                       name.c_str());
        assert(builder.created_textures().empty() && builder.created_buffers().empty());
    }

    // Resolve imported textures into graph image ids. Persistent bit is
    // sticky — once any importer marks the image persistent, it stays
    // persistent for the lifetime of the FrameGraph.
    std::vector<uint32_t> handle_to_image_id;
    handle_to_image_id.resize(builder.imported_textures().size(), UINT32_MAX);
    for (const auto& imp : builder.imported_textures()) {
        uint32_t image_id = UINT32_MAX;
        for (size_t i = 0; i < images_.size(); ++i) {
            if (images_[i].image == imp.image &&
                images_[i].base_layer == imp.base_layer &&
                images_[i].array_layers == imp.layer_count) {
                image_id = static_cast<uint32_t>(i);
                break;
            }
        }
        if (image_id == UINT32_MAX) {
            image_id = register_image(imp.image, imp.aspect,
                                       imp.base_layer, imp.layer_count,
                                       imp.mip_count,
                                       imp.persistent);
        } else if (imp.persistent) {
            images_[image_id].persistent = true;
            if (imp.mip_count > images_[image_id].mip_count) {
                // First importer undersized the mip count; upgrade.
                images_[image_id].mip_count = imp.mip_count;
                if (!images_[image_id].slices.empty()) {
                    images_[image_id].slices.front().range.mip_count = imp.mip_count;
                }
            }
        }
        if (imp.handle.id >= handle_to_image_id.size()) {
            handle_to_image_id.resize(imp.handle.id + 1, UINT32_MAX);
        }
        handle_to_image_id[imp.handle.id] = image_id;
    }

    PassDecl pass;
    pass.name = name;
    pass.execute = std::move(execute);
    pass.parallel_group = builder.parallel_group();
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
        case ImageUsage::compute_read:           return VK_IMAGE_USAGE_SAMPLED_BIT;
        case ImageUsage::compute_write:          return VK_IMAGE_USAGE_STORAGE_BIT;
        case ImageUsage::transfer_src:           return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        case ImageUsage::transfer_dst:           return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return 0;
}

} // namespace

std::vector<ResourceLifetime> FrameGraph::compute_lifetimes() const {
    std::vector<ResourceLifetime> out(images_.size());
    for (uint32_t p = 0; p < passes_.size(); ++p) {
        for (const auto& acc : passes_[p].image_uses) {
            auto& lt = out[acc.image_id];
            if (p < lt.first_pass) { lt.first_pass = p; }
            if (p > lt.last_pass || !lt.used()) { lt.last_pass = p; }
            lt.usage_flags |= usage_flag_for(acc.usage);
        }
    }
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
        if (!lt.used()) {
            FJELL_GFX_INFO("  img#{} persistent={} unused",
                           static_cast<unsigned>(i),
                           images_[i].persistent ? 1 : 0);
            continue;
        }
        FJELL_GFX_INFO("  img#{} persistent={} [{}..{}] ({} passes) usage=0x{:x} first='{}' last='{}'",
                       static_cast<unsigned>(i),
                       images_[i].persistent ? 1 : 0,
                       lt.first_pass, lt.last_pass,
                       lt.last_pass - lt.first_pass + 1,
                       static_cast<unsigned>(lt.usage_flags),
                       passes_[lt.first_pass].name.c_str(),
                       passes_[lt.last_pass].name.c_str());
    }
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
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageUsage::compute_write:
            return VK_IMAGE_LAYOUT_GENERAL;
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
        case ImageUsage::compute_write:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case ImageUsage::transfer_src:
        case ImageUsage::transfer_dst:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
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
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case ImageUsage::compute_write:
            return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case ImageUsage::transfer_src:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case ImageUsage::transfer_dst:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    return 0;
}

std::vector<size_t> FrameGraph::carve_slices(TrackedImage& img, const SubresourceRange& query) {
    // Split each overlapping slice along the query's mip/layer edges so
    // every resulting slice is either fully inside `query` or fully
    // outside. Then return the indices of the inside ones.
    SubresourceRange q = clamp_range(img, query);
    if (q.mip_count == 0 || q.layer_count == 0) { return {}; }
    const uint32_t q_mip_end = q.base_mip + q.mip_count;
    const uint32_t q_lay_end = q.base_layer + q.layer_count;

    std::vector<ImageSlice> rebuilt;
    rebuilt.reserve(img.slices.size() * 2);
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
    img.slices = std::move(rebuilt);

    std::vector<size_t> indices;
    indices.reserve(img.slices.size());
    for (size_t i = 0; i < img.slices.size(); ++i) {
        if (range_contains(q, img.slices[i].range)) {
            indices.push_back(i);
        }
    }
    return indices;
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

void FrameGraph::insert_barrier_for_slice(VkCommandBuffer cmd, const TrackedImage& img,
                                           ImageSlice& slice,
                                           VkImageLayout new_layout,
                                           VkPipelineStageFlags2 dst_stage,
                                           VkAccessFlags2 dst_access) {
    if (slice.layout == new_layout
        && (slice.last_access & dst_access) == dst_access) {
        return;
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = slice.last_stage;
    barrier.srcAccessMask = slice.last_access;
    barrier.dstStageMask = dst_stage;
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

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);

    slice.layout = new_layout;
    slice.last_stage = dst_stage;
    slice.last_access = dst_access;
}

void FrameGraph::emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass) {
    for (const auto& acc : pass.image_uses) {
        auto& img = images_[acc.image_id];
        auto needed_layout = layout_for(acc.usage, img.aspect);
        auto needed_stage = stage_for(acc.usage);
        auto needed_access = access_for(acc.usage);

        auto indices = carve_slices(img, acc.range);
        for (size_t idx : indices) {
            insert_barrier_for_slice(cmd, img, img.slices[idx],
                                      needed_layout, needed_stage, needed_access);
        }
        coalesce_slices(img);
    }
}

void FrameGraph::apply_final_layouts(const PassDecl& pass) {
    for (const auto& fl : pass.final_layouts) {
        auto& img = images_[fl.image_id];
        auto indices = carve_slices(img, fl.range);
        for (size_t idx : indices) {
            img.slices[idx].layout = fl.layout;
            img.slices[idx].last_stage = fl.last_stage;
            img.slices[idx].last_access = fl.last_access;
        }
        coalesce_slices(img);
    }
}

void FrameGraph::execute(VkCommandBuffer primary, ThreadPool* pool,
                          ThreadCommandPools* cmd_pools, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("frame_graph_execute");
    bool can_parallelize = pool && cmd_pools && pool->thread_count() > 0;

    size_t i = 0;
    while (i < passes_.size()) {
        auto& pass = passes_[i];

        if (pass.parallel_group == 0 || !can_parallelize) {
            emit_barriers_for_pass(primary, pass);
            pass.execute(primary);
            apply_final_layouts(pass);
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
            emit_barriers_for_pass(primary, passes_[p]);
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

        vkCmdExecuteCommands(primary, static_cast<uint32_t>(group_size), secondaries.data());

        for (size_t p = group_begin; p < group_begin + group_size; ++p) {
            apply_final_layouts(passes_[p]);
        }
    }
}

} // namespace fjell
