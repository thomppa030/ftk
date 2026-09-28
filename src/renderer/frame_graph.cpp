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

// FJELL_TRACE_LAYOUT=1 logs every image the graph starts tracking and what a
// pass says it leaves one in, with its handle, name and layout; the backend
// logs every barrier it records beside them.
[[nodiscard]] bool layout_trace_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("FJELL_TRACE_LAYOUT");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

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

// Clamp an externally-specified range against the image's actual
// dimensions. A count of REST reaches to the last mip or layer.
SubresourceRange clamp_range(const TrackedImage& img, SubresourceRange r) {
    constexpr uint32_t REST = gpu::TextureView::REST;
    if (r.mip_count == REST || r.base_mip + r.mip_count > img.mip_count) {
        r.mip_count = (r.base_mip < img.mip_count) ? img.mip_count - r.base_mip : 0;
    }
    if (r.layer_count == REST || r.base_layer + r.layer_count > img.array_layers) {
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
    constexpr uint32_t REST = gpu::TextureView::REST;
    const TextureShape shape = host_->shape(view.texture);
    const uint32_t base_layer = view.base_layer;
    const uint32_t layer_count =
        view.layer_count == REST ? shape.layers - base_layer : view.layer_count;
    const uint32_t mip_count =
        view.base_mip + (view.mip_count == REST ? shape.mips - view.base_mip : view.mip_count);
    const bool keep_names = tracing_ || layout_trace_enabled();
    const ImageKey key{view.texture, base_layer, layer_count};
    if (auto it = image_index_.find(key); it != image_index_.end()) {
        auto& existing = images_[it->second];
        existing.persistent = existing.persistent || persistent;
        if (existing.resting.empty()) { existing.resting = resting; }
        if (keep_names && existing.name.empty()) { existing.name = name; }
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

    const bool depth = shape.depth;
    uint32_t id = static_cast<uint32_t>(images_.size());
    TrackedImage img{};
    img.texture = view.texture;
    img.depth = depth;
    img.mip_count = mip_count;
    img.array_layers = layer_count;
    img.base_layer = base_layer;
    img.persistent = persistent;
    img.resting = resting;
    // Without a memory of it, it starts at rest once something has written
    // it; else nothing it holds is known.
    gpu::AccessSet start{};
    if (!resting.empty()) {
        bool one_state = true;
        resting.for_each([&](gpu::Access access) {
            one_state = one_state && gpu::same_state(access, resting, depth);
        });
        if (one_state) {
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
        slice.range.base_mip = 0;
        slice.range.mip_count = mip_count;
        slice.range.base_layer = base_layer;
        slice.range.layer_count = layer_count;
        slice.in = start;
        img.slices.push_back(slice);
    }
    if (keep_names) { img.name = name; }
    if (layout_trace_enabled()) {
        FJELL_GFX_INFO("[layout] register texture {} '{}' layers={}+{} mips={} start={}{}",
                       view.texture.id, name, base_layer, layer_count, mip_count,
                       host_->state_name(img.slices.front().in, depth),
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
    buf.buffer = buffer;
    buf.persistent = persistent;
    if (tracing_ || layout_trace_enabled()) { buf.name = name; }
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
    if (device_host_for_ != &device) {
        device_host_ = device_host(device);
        device_host_for_ = &device;
    }
    begin_frame(device_host_);
}

void FrameGraph::begin_frame(const GraphHost& host) {
    host_ = &host;
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
        auto& state = remembered_buffers_[buf.buffer];
        state.state = buf.state;
        state.seen = frame_serial_;
    }
}

void FrameGraph::bind_virtual_image(uint32_t image_id, gpu::Texture texture) {
    if (image_id >= images_.size()) { return; }
    auto& img = images_[image_id];
    if (!img.virtual_resource) { return; }
    img.texture = texture;
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
    // desc) resolves to the same TrackedImage. No texture until the pool
    // backs it, and one initial slice covering the whole surface so carve_slices
    // behaves normally during any stray range queries. A transient always
    // starts at UNDEFINED: its contents are dead between runs, and the
    // pool may back two of them with one image inside a run.
    for (const auto& cre : builder.created_textures()) {
        if (auto it = virtual_index_.find(cre.name_hash); it != virtual_index_.end()) {
            handle_to_image_id[cre.handle.id] = it->second;
            continue;
        }

        TrackedImage img{};
        img.depth = gpu::kind(cre.desc.format) == gpu::FormatKind::depth ||
                    gpu::kind(cre.desc.format) == gpu::FormatKind::depth_stencil;
        img.mip_count = cre.desc.mip_levels;
        img.array_layers = cre.desc.array_layers;
        img.base_layer = 0;
        img.persistent = cre.desc.persistent;
        img.virtual_resource = true;
        img.desc = cre.desc;
        img.name = cre.name;
        img.name_hash = cre.name_hash;
        ImageSlice slice{};
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

GraphHost FrameGraph::device_host(gpu::Device& device) {
    GraphHost host;
    host.shape = [&device](gpu::Texture texture) {
        const gpu::TextureInfo& info = device.info(texture);
        const VkImageAspectFlags aspect = gpu::vulkan::native_aspect(device, texture);
        return TextureShape{.mips = info.mips,
                            .layers = info.layers,
                            .depth = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0};
    };
    host.record = [&device](VkCommandBuffer cmd, gpu::Queue queue,
                            std::span<const gpu::Transition> transitions, std::string* trace) {
        gpu::vulkan::CommandBufferList commands(device, cmd, queue);
        gpu::vulkan::trace_transitions(device, trace);
        commands.list().transition(transitions);
        gpu::vulkan::trace_transitions(device, nullptr);
    };
    host.state_name = [](gpu::AccessSet state, bool depth) {
        return gpu::vulkan::layout_name(gpu::vulkan::merged_image_scope(state, depth).layout);
    };
    return host;
}

gpu::Queue FrameGraph::gpu_queue(QueueType queue) {
    return queue == QueueType::async_compute ? gpu::Queue::compute : gpu::Queue::graphics;
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

void FrameGraph::append_barrier_for_slice(const TrackedImage& img, ImageSlice& slice,
                                          gpu::AccessSet accesses, bool write, QueueType queue) {
    AccessState& s = slice.state;
    const bool layout_change = !gpu::same_state(slice.in, accesses, img.depth);
    const gpu::AccessSet writes = s.written_by | s.changed_for;

    // The same rule as for buffers, plus the layout. A layout change or a
    // write waits for the last write and every reader since; a read waits
    // for the last write unless an earlier transition already made it
    // visible to this access.
    gpu::Transition t{
        .texture = image_range(img, slice.range),
        .flush = s.written_by,
        .visible_to = accesses,
        .from = slice.in,
        .to = accesses,
        .name = img.name,
    };
    bool needed = false;
    if (layout_change || write) {
        t.wait_for = writes | s.read_by;
        needed = layout_change || !t.wait_for.empty();
    } else if (!writes.empty() &&
               !gpu::texture_already_visible(s.visible_to, accesses, img.depth, gpu_queue(queue))) {
        t.wait_for = writes;
        // Widen to everything made visible before, so the one state kept
        // covers every reader so far.
        t.visible_to = accesses | s.visible_to;
        needed = true;
    }
    if (needed) { transitions_scratch_.push_back(t); }

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
        if (needed) { s.visible_to = t.visible_to; }
        s.read_by |= accesses;
    }
    slice.in = accesses;
}

void FrameGraph::append_barrier_for_buffer(TrackedBuffer& buf, const BufferUse& use,
                                           QueueType queue) {
    AccessState& s = buf.state;

    // A write waits for the last write (write-after-write) and for every
    // reader since (write-after-read: execution only, a read leaves nothing
    // to make available). A read waits for the last write unless an earlier
    // transition already made it visible to this access.
    gpu::Transition t{
        .buffer = buf.buffer,
        .flush = s.written_by,
        .visible_to = use.access,
        .name = buf.name,
    };
    if (use.write) {
        t.wait_for = s.written_by | s.read_by;
    } else if (!s.written_by.empty() &&
               !gpu::buffer_already_visible(s.visible_to, use.access, gpu_queue(queue))) {
        t.wait_for = s.written_by;
        // Widen to everything made visible before, so the one state kept
        // covers every reader so far.
        t.visible_to = use.access | s.visible_to;
    }
    if (!t.wait_for.empty()) {
        transitions_scratch_.push_back(t);
        if (!use.write) { s.visible_to = t.visible_to; }
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
    transitions_scratch_.clear();

    for (const auto& acc : pass.image_uses) {
        auto& img = images_[acc.image_id];
        // Virtual resources are backed by a TransientImagePool allocation
        // (bound via bind_virtual_image) after compute_alias_groups runs.
        // If the pool didn't hand out a VkImage this frame, there's nothing
        // to barrier.
        if (!img.texture.valid()) { continue; }

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
            append_barrier_for_slice(img, img.slices[idx], m.accesses, m.any_write, pass.queue);
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

    record_transitions(cmd, pass.queue);
    if (tracing_) {
        barrier_trace()->file << trace_pending_;
        trace_pending_.clear();
    }
}

void FrameGraph::apply_final_states(const PassDecl& pass) {
    const bool trace = layout_trace_enabled();
    for (const auto& fs : pass.final_states) {
        auto& img = images_[fs.image_id];
        if (!img.texture.valid()) { continue; }
        if (trace || tracing_) {
            const std::string left = host_->state_name(fs.left_as, img.depth);
            if (trace) {
                FJELL_GFX_INFO("[layout] leaves pass='{}' '{}' -> {}", pass.name, img.name, left);
            }
            if (tracing_) {
                barrier_trace()->file << std::format("    leaves {} -> {}\n", img.name, left);
            }
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
    transitions_scratch_.clear();
    for (const auto& use : pass.image_uses) {
        auto& img = images_[use.image_id];
        if (img.resting.empty() || !img.texture.valid()) { continue; }
        const SubresourceRange whole{0, img.mip_count, img.base_layer, img.array_layers};
        carve_slices(img, whole, indices_scratch_);
        for (size_t idx : indices_scratch_) {
            append_barrier_for_slice(img, img.slices[idx], img.resting, false, pass.queue);
        }
        coalesce_slices(img);
    }
    record_transitions(cmd, pass.queue);
    if (tracing_ && !trace_pending_.empty()) {
        barrier_trace()->file << "  rest after " << pass.name << '\n' << trace_pending_;
        trace_pending_.clear();
    }
}

void FrameGraph::record_transitions(VkCommandBuffer cmd, QueueType queue) {
    if (transitions_scratch_.empty()) { return; }
    host_->record(cmd, gpu_queue(queue), transitions_scratch_, tracing_ ? &trace_pending_ : nullptr);
}

gpu::TextureView FrameGraph::image_range(const TrackedImage& img, const SubresourceRange& range) {
    gpu::TextureView view(img.texture);
    view.base_mip = range.base_mip;
    view.mip_count = range.mip_count;
    view.base_layer = range.base_layer;
    view.layer_count = range.layer_count;
    return view;
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
