#include "renderer/render_pipeline.hpp"
#include "renderer/fjgraph_asset.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pass/sun_shafts_pass.hpp"
#include "renderer/pass/water_pass.hpp"
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

RenderPipeline::~RenderPipeline() {
    // Exit metric: one line summarising the best aliasing win achieved
    // this session. Silent when the pipeline never ran (headless tools,
    // aborts before first frame).
    if (peak_logical_ == 0) { return; }
    const float reduction = peak_logical_ == 0 ? 0.0f
        : 100.0f * (1.0f - (static_cast<float>(peak_physical_)
                             / static_cast<float>(peak_logical_)));
    FJELL_GFX_INFO(
        "FrameGraph aliasing (peak): {} logical -> {} physical ({:.1f}% reduction, "
        "~{:.1f} MiB saved, peak VRAM {:.1f} MiB)",
        peak_logical_, peak_physical_, reduction,
        static_cast<double>(peak_bytes_saved_) / (1024.0 * 1024.0),
        static_cast<double>(peak_live_bytes_) / (1024.0 * 1024.0));
}

void RenderPipeline::register_pass(PassEntry entry) {
    pass_state_.try_emplace(entry.name); // create default state if not exists
    entry.name_hash = fg_name_hash(entry.name);
    entries_.push_back(std::move(entry));
    sorted_ = false;
}

void RenderPipeline::begin_frame() {
    graph_.new_frame();
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
                              bool async_compute_available,
                              const FjGraph* viewport_graph) {
    FJELL_PROFILE_SCOPE_N("render_pipeline_execute");

    // Per-call viewport mask. When `viewport_graph` is non-null, only
    // passes whose name appears in the graph (with `enabled: true`) run
    // this frame. This is how a SceneViewport with `editor_preview.fjgraph`
    // skips DDGI/SSR/etc on a per-call basis without touching the global
    // pass_state_ that the main viewport uses.
    auto& viewport_enabled = viewport_enabled_;
    viewport_enabled.clear();
    if (viewport_graph != nullptr) {
        for (const auto& entry : viewport_graph->passes) {
            if (entry.enabled) { viewport_enabled.insert(fg_name_hash(entry.name)); }
        }
    }
    auto pass_enabled_for_viewport = [&](const PassEntry& entry) {
        if (viewport_graph == nullptr) {
            return state_for(entry.name).enabled;
        }
        return viewport_enabled.contains(entry.name_hash);
    };

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
    // A barrier may name the task and mesh stages only on a device that has
    // them, so passes with a mesh and a vertex path declare by this.
    decl_ctx.mesh_shader_supported = ctx.draw_mesh_tasks_indirect_fn != nullptr;
    // Feature flags sourced from pass enable state — let passes that need
    // to cross-check (e.g. tonemap declaring a read on bloom_mip0) see
    // the canonical toggle without reaching into the PassRegistry.
    if (auto* bloom = passes.try_get<BloomCore>("bloom_ds_0")) {
        decl_ctx.bloom_enabled = bloom->enabled();
    }

    const bool has_msaa = (msaa != VK_SAMPLE_COUNT_1_BIT);

    // The surface depth is the scene depth with the sea laid over it, written
    // by the water as it draws. Where no water draws there is no such image
    // this frame, and the name stands for the scene depth instead — so a
    // reader never has to ask whether the scene has a sea. Decided here, from
    // this call's own context: each viewport renders a scene of its own.
    const auto* water = passes.try_get<WaterPass>("water");
    const bool water_draws = water != nullptr && water->draws(ctx);
    decl_ctx.water_draws = water_draws;
    {
        FJELL_PROFILE_SCOPE_N("rp_imports_fill");
        auto add_import = [&](std::string_view name, VkImage img, VkImageView view,
                               VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
            decl_ctx.imports.emplace(fg_name_hash(name),
                ImportedImage{img, view, aspect, 0, 1, 1, false});
        };
        add_import("color",        target.color_image(),        target.color_view());
        add_import("depth",        target.depth_image(),        target.depth_view(),
                   VK_IMAGE_ASPECT_DEPTH_BIT);
        if (water_draws) {
            add_import("surface_depth", target.surface_depth_image(),
                       target.surface_depth_view(), VK_IMAGE_ASPECT_DEPTH_BIT);
        } else {
            add_import("surface_depth", target.depth_image(), target.depth_view(),
                       VK_IMAGE_ASPECT_DEPTH_BIT);
        }
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
                decl_ctx.imports.emplace(fg_name_hash("hiz_pyramid"), img);
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
                decl_ctx.imports.emplace(fg_name_hash("gtao_ao"), img);
            }
        }
        // Fog scatter ping-pong volumes: same per-viewport-import pattern
        // as gtao_ao. The pass advances ping_pong after composite, so
        // cur_index() here points at the volume that inject/temporal/
        // integrate all target this frame.
        if (auto* fog = pp_resources->volumetric_fog()) {
            if (fog->scatter_volumes[0].view != VK_NULL_HANDLE) {
                auto add_scatter = [&](std::string_view name, uint32_t idx) {
                    ImportedImage img{};
                    img.image = fog->scatter_volumes[idx].image;
                    img.view = fog->scatter_volumes[idx].view;
                    img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                    img.base_layer = 0;
                    img.layer_count = 1;
                    img.mip_count = 1;
                    img.persistent = true;
                    decl_ctx.imports.emplace(fg_name_hash(name), img);
                };
                add_scatter("fog_scatter_cur", fog->cur_index());
                add_scatter("fog_scatter_prev", fog->prev_index());
            }
        }
    }

        // Sun shaft history ping-pong. Sized here because a resize replaces
        // the images and the graph has to be handed the ones this frame uses —
        // and only for a viewport that runs the shafts, or a thumbnail
        // rendering between two frames would resize them twice over.
        if (auto* shafts = passes.try_get<SunShaftsPass>("sun_shafts");
            shafts != nullptr && shafts->enabled()
            && (viewport_graph == nullptr ? state_for("sun_shafts").enabled
                                          : viewport_enabled.contains(fg_name_hash("sun_shafts")))) {
            auto& imgs = shafts->frame_images(
                {decl_ctx.viewport_extent.x, decl_ctx.viewport_extent.y});
            auto add_history = [&](std::string_view name, uint32_t idx) {
                ImportedImage img{};
                img.image = imgs.shafts[idx].image;
                img.view = imgs.shafts[idx].view;
                img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                img.base_layer = 0;
                img.layer_count = 1;
                img.mip_count = 1;
                img.persistent = true;
                decl_ctx.imports.emplace(fg_name_hash(name), img);
            };
            add_history(SunShaftsPass::HISTORY_CUR_NAME, imgs.cur_index());
            add_history(SunShaftsPass::HISTORY_PREV_NAME, imgs.prev_index());
        }

        passes.collect_exports_all(decl_ctx);
    }

    // ── Collect declared passes + run DAG sort ────────────────────────
    auto& pending = pending_;
    pending.clear();

    {
        FJELL_PROFILE_SCOPE_N("rp_declare_loop");
        for (size_t i = 0; i < entries_.size(); ++i) {
            auto* p = entries_[i].pass;
            if (p == nullptr) { continue; }
            if (!pass_enabled_for_viewport(entries_[i])) { continue; }
            if (pending.size() == builders_.size()) { builders_.emplace_back(); }
            PendingPass pp;
            pp.entry_index = i;
            pp.builder = pending.size();
            pp.pass = p;
            auto& builder = builders_[pp.builder];
            builder.reset();
            // Seed the builder's queue from the pass's async hint. Declare
            // can override if it wants (unusual), but the default is what
            // setup() advertised. Without an async-capable device, fall
            // through to graphics unconditionally.
            auto setup = p->setup();
            if (async_compute_available && setup.async_hint == AsyncComputeHint::Prefer) {
                builder.queue(QueueType::async_compute);
            } else {
                builder.queue(QueueType::graphics);
            }
            {
                FJELL_PROFILE_SCOPE_DYNAMIC(entries_[i].name.c_str());
                p->declare(builder, decl_ctx);
            }
            pending.push_back(pp);
        }
    }

    const size_t n = pending.size();
    auto& adj = adj_;
    for (auto& edges : adj) { edges.clear(); }
    adj.resize(n);
    auto& in_degree = in_degree_;
    in_degree.assign(n, 0);

    auto resolve_texture_key = [](const PassBuilder& b, FgTexture h) -> ResKey {
        for (const auto& imp : b.imported_textures()) {
            if (imp.handle.id == h.id) {
                return {.image = imp.image,
                        .base_layer = imp.base_layer,
                        .layer_count = imp.layer_count};
            }
        }
        for (const auto& cre : b.created_textures()) {
            if (cre.handle.id == h.id) {
                return {.virtual_name_hash = cre.name_hash};
            }
        }
        return {};
    };

    // Buffer keys: imported buffers identify by VkBuffer handle (offset/range
    // is sub-resource detail the graph doesn't model yet — coarse buffer
    // identity is enough for producer/consumer edges). Created (transient)
    // buffers key on FNV-1a hash of name, mirroring the texture path.
    auto resolve_buffer_key = [](const PassBuilder& b, FgBuffer h) -> ResKey {
        for (const auto& imp : b.imported_buffers()) {
            if (imp.handle.id == h.id) {
                return {.buffer = imp.buffer};
            }
        }
        for (const auto& cre : b.created_buffers()) {
            if (cre.handle.id == h.id) {
                return {.virtual_name_hash = cre.name_hash};
            }
        }
        return {};
    };

    auto key_valid = [](const ResKey& k) {
        return k.image != VK_NULL_HANDLE || k.buffer != VK_NULL_HANDLE
            || k.virtual_name_hash != 0;
    };

    auto& dag_order = dag_order_;
    dag_order.clear();
    {
        FJELL_PROFILE_SCOPE_N("rp_dag_sort");
        auto& last_writer = last_writer_;
        last_writer.clear();
        // Add an edge from `last_writer[k]` to pass `j` for every read
        // access whose key has been written earlier this frame. Update
        // `last_writer[k]` for every write. Buffers and textures share
        // the `ResKey` map — the discriminator is which field is set.
        auto record_access = [&](size_t j, const ResKey& k, ResourceAccess access) {
            if (!key_valid(k)) { return; }
            if (access_is_read(access)) {
                auto it = last_writer.find(k);
                if (it != last_writer.end() && it->second != j) {
                    adj[it->second].push_back(j);
                    ++in_degree[j];
                }
            }
            if (access_is_write(access)) {
                last_writer[k] = j;
            }
        };

        for (size_t j = 0; j < n; ++j) {
            const auto& b = builders_[pending[j].builder];
            for (const auto& acc : b.texture_accesses()) {
                record_access(j, resolve_texture_key(b, acc.handle), acc.access);
            }
            for (const auto& acc : b.buffer_accesses()) {
                record_access(j, resolve_buffer_key(b, acc.handle), acc.access);
            }
        }

        // Kahn's algorithm. Tiebreaker = original entry index — preserves
        // the legacy phase+sub_order ordering for passes the DAG can't
        // constrain (e.g. empty-access viz passes).
        auto cmp = [&pending](size_t a, size_t b) {
            return pending[a].entry_index > pending[b].entry_index;
        };
        std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);
        for (size_t i = 0; i < n; ++i) {
            if (in_degree[i] == 0) { pq.push(i); }
        }
        while (!pq.empty()) {
            size_t idx = pq.top();
            pq.pop();
            dag_order.push_back(idx);
            for (size_t next : adj[idx]) {
                if (--in_degree[next] == 0) { pq.push(next); }
            }
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
    // Hi-Z, sky cubemap — consumed by this frame through import_named,
    // but also by next frame's read on the same image/buffer).
    //
    // Both texture and buffer accesses are walked. Passes that update
    // pass-private textures or buffers bound through the global
    // descriptor set still need has_side_effects() — those consumer
    // edges live outside the DAG.
    auto& alive = alive_;
    alive.assign(n, 0);
    {
        FJELL_PROFILE_SCOPE_N("rp_pass_cull");
        auto& work = work_;
        work.clear();

        // Collect writers of each resource for the backward walk.
        auto& writers_of = writers_of_;
        writers_of.clear();
        for (size_t j = 0; j < n; ++j) {
            const auto& b = builders_[pending[j].builder];
            for (const auto& acc : b.texture_accesses()) {
                if (!access_is_write(acc.access)) { continue; }
                ResKey k = resolve_texture_key(b, acc.handle);
                if (!key_valid(k)) { continue; }
                writers_of[k].push_back(j);
            }
            for (const auto& acc : b.buffer_accesses()) {
                if (!access_is_write(acc.access)) { continue; }
                ResKey k = resolve_buffer_key(b, acc.handle);
                if (!key_valid(k)) { continue; }
                writers_of[k].push_back(j);
            }
        }

        // Seed the set with roots.
        auto pass_writes_persistent = [](const PassBuilder& b) {
            for (const auto& imp : b.imported_textures()) {
                if (!imp.persistent) { continue; }
                for (const auto& acc : b.texture_accesses()) {
                    if (acc.handle.id == imp.handle.id && access_is_write(acc.access)) {
                        return true;
                    }
                }
            }
            for (const auto& imp : b.imported_buffers()) {
                if (!imp.persistent) { continue; }
                for (const auto& acc : b.buffer_accesses()) {
                    if (acc.handle.id == imp.handle.id && access_is_write(acc.access)) {
                        return true;
                    }
                }
            }
            return false;
        };

        for (size_t j = 0; j < n; ++j) {
            const auto& b = builders_[pending[j].builder];
            bool root = b.is_never_cull() || b.has_side_effects_flag()
                     || pass_writes_persistent(b);
            if (root) {
                alive[j] = 1;
                work.push_back(j);
            }
        }

        while (!work.empty()) {
            size_t j = work.back();
            work.pop_back();
            const auto& b = builders_[pending[j].builder];
            auto walk_back = [&](const ResKey& k) {
                if (!key_valid(k)) { return; }
                auto it = writers_of.find(k);
                if (it == writers_of.end()) { return; }
                for (size_t w : it->second) {
                    if (alive[w]) { continue; }
                    alive[w] = 1;
                    work.push_back(w);
                }
            };
            for (const auto& acc : b.texture_accesses()) {
                if (!access_is_read(acc.access)) { continue; }
                walk_back(resolve_texture_key(b, acc.handle));
            }
            for (const auto& acc : b.buffer_accesses()) {
                if (!access_is_read(acc.access)) { continue; }
                walk_back(resolve_buffer_key(b, acc.handle));
            }
        }
    }

    size_t culled = 0;
    for (uint8_t a : alive) { if (!a) { ++culled; } }

    if (!dag_order_logged_) {
        FJELL_GFX_INFO("RenderPipeline driven by declare(): {} passes ({} culled).",
                       static_cast<unsigned>(n), static_cast<unsigned>(culled));
        dag_order_logged_ = true;
    }

    // ── Dispatch to the FrameGraph ────────────────────────────────────
    auto& graph = graph_;
    graph.set_aliasing_enabled(aliasing_enabled_);
    {
    FJELL_PROFILE_SCOPE_N("rp_graph_submit");
    graph.begin_frame();
    auto color_id        = graph.register_image(target.color_image());
    auto msaa_color_id   = has_msaa ? graph.register_image(target.msaa_color_image()) : color_id;
    auto depth_id        = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    auto msaa_depth_id   = has_msaa ? graph.register_image(target.msaa_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT) : depth_id;
    auto surface_depth_id = water_draws ? graph.register_image(target.surface_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT) : depth_id;
    auto mr_id           = graph.register_image(target.mr_image());
    auto msaa_mr_id      = has_msaa ? graph.register_image(target.msaa_mr_image()) : mr_id;
    auto normal_id       = graph.register_image(target.normal_image());
    auto msaa_normal_id  = has_msaa ? graph.register_image(target.msaa_normal_image()) : normal_id;
    auto velocity_id     = graph.register_image(target.velocity_image());
    auto msaa_velocity_id = has_msaa ? graph.register_image(target.msaa_velocity_image()) : velocity_id;
    auto screen_color_id = graph.register_image(target.screen_color_image());
    auto distortion_id   = graph.register_image(target.distortion_image());

    registry.register_image("color",         {.frame_graph_id = color_id,         .image = target.color_image(),         .view = target.color_view()});
    registry.register_image("msaa_color",    {.frame_graph_id = msaa_color_id,    .image = target.msaa_color_image(),    .view = target.msaa_color_view()});
    registry.register_image("depth",         {.frame_graph_id = depth_id,         .image = target.depth_image(),         .view = target.depth_view()});
    registry.register_image("msaa_depth",    {.frame_graph_id = msaa_depth_id,    .image = target.msaa_depth_image(),    .view = target.msaa_depth_view()});
    registry.register_image("surface_depth", {.frame_graph_id = surface_depth_id,
                                              .image = water_draws ? target.surface_depth_image() : target.depth_image(),
                                              .view = water_draws ? target.surface_depth_view() : target.depth_view()});
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
            builders_[pp.builder],
            [p = pp.pass, &ctx](VkCommandBuffer cb) {
                FrameContext local = ctx;
                local.cmd = cb;
                p->record(local);
            });
    }
    } // rp_graph_submit

    // Allocate a VkImage per alias group from the cross-frame pool and
    // register each logical resource under the name it was declared
    // with. Passes rewire their record() reads through
    // ctx.registry->image_view(name) in C3.3+.
    if (ctx.transient_pool != nullptr) {
        FJELL_PROFILE_SCOPE_N("rp_alias_bind");
        auto& lifetimes = lifetimes_;
        auto& groups = groups_;
        graph.compute_lifetimes(lifetimes);
        graph.compute_alias_groups(lifetimes, groups);
        const auto& imgs = graph.images();

        // Snapshot + peak tracking for debug UI and shutdown summary.
        uint32_t logical_count = 0;
        for (const auto& img : imgs) {
            if (img.virtual_resource && !img.persistent) { ++logical_count; }
        }
        aliasing_stats_.logical_count = logical_count;
        aliasing_stats_.physical_count = static_cast<uint32_t>(groups.size());

        if (logical_count > peak_logical_) { peak_logical_ = logical_count; }
        if (static_cast<uint32_t>(groups.size()) > peak_physical_) {
            peak_physical_ = static_cast<uint32_t>(groups.size());
        }

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

        aliasing_stats_.live_images = ctx.transient_pool->live_images();
        aliasing_stats_.live_bytes = ctx.transient_pool->live_bytes();
        aliasing_stats_.acquires = ctx.transient_pool->acquires_this_frame();
        aliasing_stats_.allocations = ctx.transient_pool->allocations_this_frame();

        if (aliasing_stats_.live_bytes > peak_live_bytes_) {
            peak_live_bytes_ = aliasing_stats_.live_bytes;
        }

        // Track the best "bytes saved" seen this session. We compare the
        // physical VRAM that holds `physical_count` images against the
        // hypothetical unaliased cost of `logical_count` images of the
        // same average size. Conservative: assumes all logicals share the
        // average — close enough for a rough win metric.
        if (aliasing_stats_.physical_count > 0 && aliasing_stats_.logical_count > 0
            && aliasing_stats_.live_bytes > 0) {
            const uint64_t avg_bytes = aliasing_stats_.live_bytes
                / aliasing_stats_.physical_count;
            const uint64_t hypothetical = avg_bytes * aliasing_stats_.logical_count;
            if (hypothetical > aliasing_stats_.live_bytes) {
                const uint64_t saved = hypothetical - aliasing_stats_.live_bytes;
                if (saved > peak_bytes_saved_) { peak_bytes_saved_ = saved; }
            }
        }
    }

    if (!lifetimes_logged_) {
        graph.log_lifetimes();
        graph.log_alias_groups();
        graph.log_queue_segments();
        (void)graph.validate_alias_groups(graph.compute_alias_groups());
        if (ctx.transient_pool != nullptr) {
            FJELL_GFX_DEBUG(
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
