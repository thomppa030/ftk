#include "renderer/render_pipeline.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace fjell {

const RenderPipeline::PassState RenderPipeline::default_state_{};

void RenderPipeline::register_pass(PassEntry entry) {
    pass_state_.try_emplace(entry.name); // create default state if not exists
    entries_.push_back(std::move(entry));
    sorted_ = false;
}

void RenderPipeline::remove_pass(const std::string& name) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                        [&](const PassEntry& e) { return e.name == name; }),
        entries_.end());
    pass_state_.erase(name);
}

void RenderPipeline::set_pass_enabled(const std::string& name, bool enabled) {
    pass_state_[name].enabled = enabled;
}

bool RenderPipeline::is_pass_enabled(const std::string& name) const {
    return state_for(name).enabled;
}

void RenderPipeline::set_pass_resources(const std::string& name,
                                         std::vector<std::string> reads,
                                         std::vector<std::string> writes) {
    auto& s = pass_state_[name];
    s.reads = std::move(reads);
    s.writes = std::move(writes);
}

bool RenderPipeline::is_resource_available(const std::string& resource) const {
    for (const auto& [name, state] : pass_state_) {
        if (!state.enabled) continue;
        for (const auto& w : state.writes) {
            if (w == resource) return true;
        }
    }
    return false;
}

RenderPipeline::PassState& RenderPipeline::state_for(const std::string& name) {
    return pass_state_[name];
}

const RenderPipeline::PassState& RenderPipeline::state_for(const std::string& name) const {
    auto it = pass_state_.find(name);
    if (it != pass_state_.end()) return it->second;
    return default_state_;
}

void RenderPipeline::execute(VkCommandBuffer cmd, OffscreenPass& target,
                              VkSampleCountFlagBits msaa, uint32_t active_sources,
                              ThreadPool& pool, ThreadCommandPools& secondary_pools,
                              uint32_t frame_index, ResourceRegistry& registry,
                              PassRegistry& passes, PostProcessResources* pp_resources,
                              const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("render_pipeline_execute");

    // Topological sort by resource dependencies, order as tiebreaker
    if (!sorted_) {
        size_t n = entries_.size();

        // Build adjacency: if pass i writes resource R and pass j reads R, add edge i→j
        std::vector<std::vector<size_t>> adj(n);
        std::vector<uint32_t> in_degree(n, 0);

        // Map resource name → indices of passes that write it
        std::unordered_map<std::string, std::vector<size_t>> writers;
        for (size_t i = 0; i < n; ++i) {
            const auto& s = state_for(entries_[i].name);
            for (const auto& w : s.writes) {
                writers[w].push_back(i);
            }
        }

        // For each pass that reads a resource, add edges from all its writers
        // Only add edge if the writer has a lower order (respects intended ordering
        // when multiple passes write the same resource in sequence)
        for (size_t j = 0; j < n; ++j) {
            const auto& s = state_for(entries_[j].name);
            for (const auto& r : s.reads) {
                auto it = writers.find(r);
                if (it == writers.end()) continue;
                for (size_t i : it->second) {
                    if (i == j) continue;
                    // Only add edge from writer → reader if writer has lower order
                    if (entries_[i].order < entries_[j].order) {
                        adj[i].push_back(j);
                        in_degree[j]++;
                    }
                }
            }
        }

        // Kahn's algorithm with priority queue (order as tiebreaker)
        auto cmp = [this](size_t a, size_t b) {
            return entries_[a].order > entries_[b].order; // min-heap
        };
        std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);

        for (size_t i = 0; i < n; ++i) {
            if (in_degree[i] == 0) pq.push(i);
        }

        std::vector<PassEntry> sorted;
        sorted.reserve(n);
        while (!pq.empty()) {
            size_t idx = pq.top();
            pq.pop();
            sorted.push_back(std::move(entries_[idx]));
            for (size_t next : adj[idx]) {
                if (--in_degree[next] == 0) pq.push(next);
            }
        }

        entries_ = std::move(sorted);
        sorted_ = true;
    }

    // ── Per-frame DeclareContext ──────────────────────────────────────
    DeclareContext decl_ctx;
    decl_ctx.viewport_extent = {target.extent().width, target.extent().height};
    decl_ctx.msaa_samples    = msaa;
    decl_ctx.frame_index     = frame_index;
    decl_ctx.active_sources  = active_sources;

    const bool has_msaa = (msaa != VK_SAMPLE_COUNT_1_BIT);
    auto add_import = [&](std::string name, VkImage img, VkImageView view,
                           VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        decl_ctx.imports.emplace(std::move(name),
            ImportedImage{img, view, aspect, 0, 1, 1, false});
    };
    add_import("color",        target.color_image(),        target.color_view());
    add_import("depth",        target.depth_image(),        target.depth_view(),
               VK_IMAGE_ASPECT_DEPTH_BIT);
    add_import("mr",           target.mr_image(),           target.mr_view());
    add_import("normal",       target.normal_image(),       target.normal_view());
    add_import("velocity",     target.velocity_image(),     target.velocity_view());
    add_import("screen_color", target.screen_color_image(), target.screen_color_view());
    add_import("distortion",   target.distortion_image(),   target.distortion_view());
    if (has_msaa) {
        add_import("msaa_color",    target.msaa_color_image(),    target.msaa_color_view());
        add_import("msaa_depth",    target.msaa_depth_image(),    target.msaa_depth_view(),
                   VK_IMAGE_ASPECT_DEPTH_BIT);
        add_import("msaa_mr",       target.msaa_mr_image(),       target.msaa_mr_view());
        add_import("msaa_normal",   target.msaa_normal_image(),   target.msaa_normal_view());
        add_import("msaa_velocity", target.msaa_velocity_image(), target.msaa_velocity_view());
    } else {
        // MSAA off: the msaa_* slots alias the non-MSAA images so
        // passes that unconditionally import by the msaa name still
        // resolve to a valid VkImage.
        add_import("msaa_color",    target.color_image(),    target.color_view());
        add_import("msaa_depth",    target.depth_image(),    target.depth_view(),
                   VK_IMAGE_ASPECT_DEPTH_BIT);
        add_import("msaa_mr",       target.mr_image(),       target.mr_view());
        add_import("msaa_normal",   target.normal_image(),   target.normal_view());
        add_import("msaa_velocity", target.velocity_image(), target.velocity_view());
    }

    if (pp_resources != nullptr) {
        if (auto* hiz = pp_resources->hiz()) {
            if (hiz->ready() && hiz->hiz_sampled_view != VK_NULL_HANDLE) {
                ImportedImage img{};
                img.image = hiz->hiz_image;
                img.view = hiz->hiz_sampled_view;
                img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                img.base_layer = 0;
                img.layer_count = 1;
                img.mip_count = hiz->hiz_mip_count;
                img.persistent = true;
                decl_ctx.imports.emplace("hiz_pyramid", img);
            }
        }
    }

    passes.collect_exports_all(decl_ctx);

    // ── Collect declared passes + run DAG sort ────────────────────────
    struct PendingPass {
        size_t entry_index{0};
        PassBuilder builder;
        RenderPass* pass{nullptr};
    };
    std::vector<PendingPass> pending;
    pending.reserve(entries_.size());

    for (size_t i = 0; i < entries_.size(); ++i) {
        auto* p = entries_[i].pass;
        if (p == nullptr) { continue; }
        if (!state_for(entries_[i].name).enabled) { continue; }
        PendingPass pp;
        pp.entry_index = i;
        pp.pass = p;
        p->declare(pp.builder, decl_ctx);
        pending.push_back(std::move(pp));
    }

    // Resource identity for topological sort: (VkImage, base_layer,
    // layer_count). Builder-local created handles aren't represented
    // yet; they surface in Phase 3 with transient aliasing.
    struct ResKey {
        VkImage image;
        uint32_t base_layer;
        uint32_t layer_count;
        bool operator==(const ResKey& o) const noexcept {
            return image == o.image && base_layer == o.base_layer
                && layer_count == o.layer_count;
        }
    };
    struct ResKeyHash {
        size_t operator()(const ResKey& k) const noexcept {
            auto h1 = std::hash<void*>{}(static_cast<void*>(k.image));
            auto h2 = std::hash<uint32_t>{}(k.base_layer);
            auto h3 = std::hash<uint32_t>{}(k.layer_count);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    const size_t n = pending.size();
    std::vector<std::vector<size_t>> adj(n);
    std::vector<uint32_t> in_degree(n, 0);

    auto resolve_key = [](const PassBuilder& b, FgTexture h) -> ResKey {
        for (const auto& imp : b.imported_textures()) {
            if (imp.handle.id == h.id) {
                return {imp.image, imp.base_layer, imp.layer_count};
            }
        }
        return {VK_NULL_HANDLE, 0, 0};
    };

    std::unordered_map<ResKey, size_t, ResKeyHash> last_writer;
    for (size_t j = 0; j < n; ++j) {
        const auto& b = pending[j].builder;
        for (const auto& acc : b.texture_accesses()) {
            ResKey k = resolve_key(b, acc.handle);
            if (k.image == VK_NULL_HANDLE) { continue; }
            if (access_is_read(acc.access)) {
                auto it = last_writer.find(k);
                if (it != last_writer.end() && it->second != j) {
                    adj[it->second].push_back(j);
                    ++in_degree[j];
                }
            }
            if (access_is_write(acc.access)) {
                last_writer[k] = j;
            }
        }
    }

    // Kahn's algorithm. Tiebreaker = original entry index — preserves
    // the legacy phase+sub_order ordering for passes the DAG can't
    // constrain (empty-access viz passes, SSBO-only passes waiting on
    // Phase 3 buffer tracking).
    auto cmp = [&pending](size_t a, size_t b) {
        return pending[a].entry_index > pending[b].entry_index;
    };
    std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);
    for (size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) { pq.push(i); }
    }
    std::vector<size_t> dag_order;
    dag_order.reserve(n);
    while (!pq.empty()) {
        size_t idx = pq.top();
        pq.pop();
        dag_order.push_back(idx);
        for (size_t next : adj[idx]) {
            if (--in_degree[next] == 0) { pq.push(next); }
        }
    }

    if (dag_order.size() != n) {
        FJELL_GFX_ERROR(
            "RenderPipeline: cycle detected in declared graph ({} of {} passes scheduled).",
            static_cast<unsigned>(dag_order.size()), static_cast<unsigned>(n));
        return;
    }

    // ── Pass culling ──────────────────────────────────────────────────
    // Walk backwards from required passes and keep anything they depend
    // on. A pass is required when it declares never_cull(), when it
    // flags has_side_effects(), or when it writes to a persistent
    // resource whose state must carry across frames (shadow atlases,
    // Hi-Z, sky cubemap — consumed by this frame through
    // import_named, but also by next frame's read on the same images).
    //
    // Rationale: the graph only tracks images, so most real
    // side-effects (SSBO writes, pass-private texture updates,
    // descriptor writes into the bindless set, host readbacks) are
    // invisible to the DAG. Passes that do those have to mark
    // has_side_effects() explicitly. Anything else that isn't reached
    // from a root can be dropped — the output it produced would have
    // been thrown away anyway.
    std::vector<bool> alive(n, false);
    {
        std::vector<size_t> work;
        work.reserve(n);

        // Collect writers of each resource for the backward walk.
        std::unordered_map<ResKey, std::vector<size_t>, ResKeyHash> writers_of;
        for (size_t j = 0; j < n; ++j) {
            const auto& b = pending[j].builder;
            for (const auto& acc : b.texture_accesses()) {
                if (!access_is_write(acc.access)) { continue; }
                ResKey k = resolve_key(b, acc.handle);
                if (k.image == VK_NULL_HANDLE) { continue; }
                writers_of[k].push_back(j);
            }
        }

        // Seed the set with roots.
        for (size_t j = 0; j < n; ++j) {
            const auto& b = pending[j].builder;
            bool root = b.is_never_cull() || b.has_side_effects_flag();
            if (!root) {
                for (const auto& imp : b.imported_textures()) {
                    for (const auto& acc : b.texture_accesses()) {
                        if (acc.handle.id != imp.handle.id) { continue; }
                        if (!access_is_write(acc.access)) { continue; }
                        if (imp.persistent) {
                            root = true;
                            break;
                        }
                    }
                    if (root) { break; }
                }
            }
            if (root) {
                alive[j] = true;
                work.push_back(j);
            }
        }

        while (!work.empty()) {
            size_t j = work.back();
            work.pop_back();
            const auto& b = pending[j].builder;
            for (const auto& acc : b.texture_accesses()) {
                if (!access_is_read(acc.access)) { continue; }
                ResKey k = resolve_key(b, acc.handle);
                if (k.image == VK_NULL_HANDLE) { continue; }
                auto it = writers_of.find(k);
                if (it == writers_of.end()) { continue; }
                for (size_t w : it->second) {
                    if (alive[w]) { continue; }
                    alive[w] = true;
                    work.push_back(w);
                }
            }
        }
    }

    size_t culled = 0;
    for (bool a : alive) { if (!a) { ++culled; } }

    if (!dag_order_logged_) {
        FJELL_GFX_INFO("RenderPipeline driven by declare(): {} passes ({} culled).",
                       static_cast<unsigned>(n), static_cast<unsigned>(culled));
        dag_order_logged_ = true;
    }

    // ── Dispatch to the FrameGraph ────────────────────────────────────
    FrameGraph graph;
    auto color_id        = graph.register_image(target.color_image());
    auto msaa_color_id   = has_msaa ? graph.register_image(target.msaa_color_image()) : color_id;
    auto depth_id        = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    auto msaa_depth_id   = has_msaa ? graph.register_image(target.msaa_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT) : depth_id;
    auto mr_id           = graph.register_image(target.mr_image());
    auto msaa_mr_id      = has_msaa ? graph.register_image(target.msaa_mr_image()) : mr_id;
    auto normal_id       = graph.register_image(target.normal_image());
    auto msaa_normal_id  = has_msaa ? graph.register_image(target.msaa_normal_image()) : normal_id;
    auto velocity_id     = graph.register_image(target.velocity_image());
    auto msaa_velocity_id = has_msaa ? graph.register_image(target.msaa_velocity_image()) : velocity_id;
    auto screen_color_id = graph.register_image(target.screen_color_image());
    auto distortion_id   = graph.register_image(target.distortion_image());

    graph.begin_frame();

    registry.register_image("color", {color_id, target.color_view()});
    registry.register_image("msaa_color", {msaa_color_id, target.msaa_color_view()});
    registry.register_image("depth", {depth_id, target.depth_view()});
    registry.register_image("msaa_depth", {msaa_depth_id, target.msaa_depth_view()});
    registry.register_image("mr", {mr_id, target.mr_view()});
    registry.register_image("msaa_mr", {msaa_mr_id, target.msaa_mr_view()});
    registry.register_image("normal", {normal_id, target.normal_view()});
    registry.register_image("msaa_normal", {msaa_normal_id, target.msaa_normal_view()});
    registry.register_image("velocity", {velocity_id, target.velocity_view()});
    registry.register_image("msaa_velocity", {msaa_velocity_id, target.msaa_velocity_view()});
    registry.register_image("screen_color", {screen_color_id, target.screen_color_view()});
    registry.register_image("distortion", {distortion_id, target.distortion_view()});

    for (size_t idx : dag_order) {
        if (!alive[idx]) { continue; }
        auto& pp = pending[idx];
        graph.submit_declared_pass(
            entries_[pp.entry_index].name,
            pp.builder,
            [p = pp.pass, &ctx](VkCommandBuffer cb) {
                FrameContext local = ctx;
                local.cmd = cb;
                p->record(local);
            });
    }

    graph.execute(cmd, &pool, &secondary_pools, frame_index);
}

} // namespace fjell
