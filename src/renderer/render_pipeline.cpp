#include "renderer/render_pipeline.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/transient_image_pool.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <queue>

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

RenderPipeline::PassState& RenderPipeline::state_for(const std::string& name) {
    return pass_state_[name];
}

const RenderPipeline::PassState& RenderPipeline::state_for(const std::string& name) const {
    auto it = pass_state_.find(name);
    if (it != pass_state_.end()) return it->second;
    return default_state_;
}

bool RenderPipeline::execute(VkCommandBuffer cmd, OffscreenPass& target,
                              VkSampleCountFlagBits msaa, uint32_t active_sources,
                              ThreadPool& pool, ThreadCommandPools& secondary_pools,
                              uint32_t frame_index, ResourceRegistry& registry,
                              PassRegistry& passes, PostProcessResources* pp_resources,
                              const FrameContext& ctx,
                              bool async_compute_available) {
    FJELL_PROFILE_SCOPE_N("render_pipeline_execute");

    // One-shot ordering pass by entry.order. The real topological sort
    // happens below on the declared DAG; this only fixes a deterministic
    // initial order for use as the DAG sort's tiebreaker when two passes
    // have no resource-derived dependency between them.
    if (!sorted_) {
        std::sort(entries_.begin(), entries_.end(),
                  [](const PassEntry& a, const PassEntry& b) {
                      return a.order < b.order;
                  });
        sorted_ = true;
    }

    // ── Per-frame DeclareContext ──────────────────────────────────────
    DeclareContext decl_ctx;
    decl_ctx.viewport_extent = {target.extent().width, target.extent().height};
    decl_ctx.msaa_samples    = msaa;
    decl_ctx.frame_index     = frame_index;
    decl_ctx.active_sources  = active_sources;
    // Feature flags sourced from pass enable state — let passes that need
    // to cross-check (e.g. tonemap declaring a read on bloom_mip0) see
    // the canonical toggle without reaching into the PassRegistry.
    if (auto* bloom = passes.try_get<BloomCore>("bloom_ds_0")) {
        decl_ctx.bloom_enabled = bloom->enabled();
    }

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
        // AO output image is per-viewport: look up the active pp_resources
        // ao_image here so declare-time imports match whichever viewport
        // is being recorded. AoPass::collect_exports() can't see
        // pp_resources, and default_pp_'s ao_image isn't the one the
        // preview viewport writes — importing the wrong one lets the
        // graph tracker's layout drift from actual image state.
        if (auto* gtao = pp_resources->gtao()) {
            if (gtao->ao_image != VK_NULL_HANDLE && gtao->ao_view != VK_NULL_HANDLE) {
                ImportedImage img{};
                img.image = gtao->ao_image;
                img.view = gtao->ao_view;
                img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                img.base_layer = 0;
                img.layer_count = 1;
                img.mip_count = 1;
                img.persistent = true;
                decl_ctx.imports.emplace("gtao_ao", img);
            }
        }
        // Fog scatter ping-pong volumes: same per-viewport-import pattern
        // as gtao_ao. The pass advances ping_pong after composite, so
        // cur_index() here points at the volume that inject/temporal/
        // integrate all target this frame.
        if (auto* fog = pp_resources->volumetric_fog()) {
            if (fog->scatter_volumes[0].view != VK_NULL_HANDLE) {
                auto add_scatter = [&](const char* name, uint32_t idx) {
                    ImportedImage img{};
                    img.image = fog->scatter_volumes[idx].image;
                    img.view = fog->scatter_volumes[idx].view;
                    img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                    img.base_layer = 0;
                    img.layer_count = 1;
                    img.mip_count = 1;
                    img.persistent = true;
                    decl_ctx.imports.emplace(name, img);
                };
                add_scatter("fog_scatter_cur", fog->cur_index());
                add_scatter("fog_scatter_prev", fog->prev_index());
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
        // Seed the builder's queue from the pass's async hint. Declare
        // can override if it wants (unusual), but the default is what
        // setup() advertised. Without an async-capable device, fall
        // through to graphics unconditionally.
        auto setup = p->setup();
        if (async_compute_available && setup.async_hint == AsyncComputeHint::Prefer) {
            pp.builder.queue(QueueType::async_compute);
        } else {
            pp.builder.queue(QueueType::graphics);
        }
        p->declare(pp.builder, decl_ctx);
        pending.push_back(std::move(pp));
    }

    // Resource identity for topological sort. Imported images are keyed by
    // (VkImage, base_layer, layer_count) — stable across passes for the
    // same physical resource. Created (transient) resources are keyed by
    // the name they were declared with, so two passes referencing the same
    // logical transient (one calls create("foo"), the other reads "foo" by
    // create'ing it again in its declare, or vice versa) resolve to the
    // same key and the DAG sort / liveness walk sees the edge.
    struct ResKey {
        VkImage image{VK_NULL_HANDLE};
        uint32_t base_layer{0};
        uint32_t layer_count{0};
        std::string virtual_name;  // non-empty iff image == VK_NULL_HANDLE
        bool operator==(const ResKey& o) const noexcept {
            return image == o.image && base_layer == o.base_layer
                && layer_count == o.layer_count && virtual_name == o.virtual_name;
        }
    };
    struct ResKeyHash {
        size_t operator()(const ResKey& k) const noexcept {
            auto h1 = std::hash<void*>{}(static_cast<void*>(k.image));
            auto h2 = std::hash<uint32_t>{}(k.base_layer);
            auto h3 = std::hash<uint32_t>{}(k.layer_count);
            auto h4 = std::hash<std::string>{}(k.virtual_name);
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
        }
    };

    const size_t n = pending.size();
    std::vector<std::vector<size_t>> adj(n);
    std::vector<uint32_t> in_degree(n, 0);

    auto resolve_key = [](const PassBuilder& b, FgTexture h) -> ResKey {
        for (const auto& imp : b.imported_textures()) {
            if (imp.handle.id == h.id) {
                return {imp.image, imp.base_layer, imp.layer_count, {}};
            }
        }
        for (const auto& cre : b.created_textures()) {
            if (cre.handle.id == h.id) {
                return {VK_NULL_HANDLE, 0, 0, cre.name};
            }
        }
        return {VK_NULL_HANDLE, 0, 0, {}};
    };

    auto key_valid = [](const ResKey& k) {
        return k.image != VK_NULL_HANDLE || !k.virtual_name.empty();
    };

    std::unordered_map<ResKey, size_t, ResKeyHash> last_writer;
    for (size_t j = 0; j < n; ++j) {
        const auto& b = pending[j].builder;
        for (const auto& acc : b.texture_accesses()) {
            ResKey k = resolve_key(b, acc.handle);
            if (!key_valid(k)) { continue; }
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
        return false;
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
                if (!key_valid(k)) { continue; }
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
                if (!key_valid(k)) { continue; }
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

    registry.register_image("color",         {.frame_graph_id = color_id,         .image = target.color_image(),         .view = target.color_view()});
    registry.register_image("msaa_color",    {.frame_graph_id = msaa_color_id,    .image = target.msaa_color_image(),    .view = target.msaa_color_view()});
    registry.register_image("depth",         {.frame_graph_id = depth_id,         .image = target.depth_image(),         .view = target.depth_view()});
    registry.register_image("msaa_depth",    {.frame_graph_id = msaa_depth_id,    .image = target.msaa_depth_image(),    .view = target.msaa_depth_view()});
    registry.register_image("mr",            {.frame_graph_id = mr_id,            .image = target.mr_image(),            .view = target.mr_view()});
    registry.register_image("msaa_mr",       {.frame_graph_id = msaa_mr_id,       .image = target.msaa_mr_image(),       .view = target.msaa_mr_view()});
    registry.register_image("normal",        {.frame_graph_id = normal_id,        .image = target.normal_image(),        .view = target.normal_view()});
    registry.register_image("msaa_normal",   {.frame_graph_id = msaa_normal_id,   .image = target.msaa_normal_image(),   .view = target.msaa_normal_view()});
    registry.register_image("velocity",      {.frame_graph_id = velocity_id,      .image = target.velocity_image(),      .view = target.velocity_view()});
    registry.register_image("msaa_velocity", {.frame_graph_id = msaa_velocity_id, .image = target.msaa_velocity_image(), .view = target.msaa_velocity_view()});
    registry.register_image("screen_color",  {.frame_graph_id = screen_color_id,  .image = target.screen_color_image(),  .view = target.screen_color_view()});
    registry.register_image("distortion",    {.frame_graph_id = distortion_id,    .image = target.distortion_image(),    .view = target.distortion_view()});

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

    // Allocate a VkImage per alias group from the cross-frame pool and
    // register each logical resource under the name it was declared
    // with. Passes rewire their record() reads through
    // ctx.registry->image_view(name) in C3.3+.
    if (ctx.transient_pool != nullptr) {
        ctx.transient_pool->begin_frame(target.extent());
        const auto groups = graph.compute_alias_groups();
        const auto lifetimes = graph.compute_lifetimes();
        const auto& imgs = graph.images();
        for (const auto& group : groups) {
            VkImageUsageFlags usage_flags = 0;
            for (uint32_t rid : group.resource_ids) {
                usage_flags |= lifetimes[rid].usage_flags;
            }
            const auto* alloc = ctx.transient_pool->acquire(
                group.desc, target.extent(), usage_flags);
            if (alloc == nullptr) { continue; }
            for (uint32_t rid : group.resource_ids) {
                const auto& img = imgs[rid];
                if (img.name.empty()) { continue; }
                ResourceRegistry::ImageResource res{};
                res.frame_graph_id = rid;
                res.image = alloc->image;
                res.view = alloc->full_view;
                res.sampler = VK_NULL_HANDLE;
                res.mip_count = img.mip_count;
                registry.register_image(img.name, res);
                // Tell the graph which VkImage backs this virtual resource
                // this frame, so emit_barriers_for_pass has something to
                // transition. Slice state is already seeded to UNDEFINED
                // by begin_frame(), which is what we want — the first
                // access will barrier out of UNDEFINED regardless of
                // whether the pool handed back a fresh or reused image.
                graph.bind_virtual_image(rid, alloc->image);
            }
        }
    }

    if (!lifetimes_logged_) {
        graph.log_lifetimes();
        graph.log_alias_groups();
        graph.log_queue_segments();
        if (ctx.transient_pool != nullptr) {
            FJELL_GFX_INFO(
                "TransientImagePool: {} live, {} acquires, {} new allocations this frame",
                ctx.transient_pool->live_images(),
                ctx.transient_pool->acquires_this_frame(),
                ctx.transient_pool->allocations_this_frame());
        }
        lifetimes_logged_ = true;
    }

    // Async compute CB + post-compute graphics CB are provided by the
    // caller via ctx. Null handles here (no async support, or preview
    // renderer) cause FrameGraph to fold everything back into the
    // graphics CB — same as single-CB behavior.
    return graph.execute(cmd, ctx.graphics_post_cmd, ctx.async_compute_cmd,
                         &pool, &secondary_pools, frame_index);
}

} // namespace fjell
