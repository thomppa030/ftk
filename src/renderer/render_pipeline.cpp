#include "renderer/render_pipeline.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/fjgraph_asset.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/pass/sun_shafts_pass.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass/water_pass.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/systems/render_subsystem.hpp"
#include "renderer/transient_image_pool.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "scene/scene.hpp"
#include "gpu/vulkan/native.hpp"
#include "gpu/vulkan/translate.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <gpu/device.hpp>

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
                              gpu::Samples msaa, uint32_t active_sources,
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
    decl_ctx.scene_id        = ctx.scene != nullptr ? ctx.scene->id() : 0;
    decl_ctx.active_sources  = active_sources;
    decl_ctx.grid_shown      = ctx.show_grid;
    decl_ctx.debug_lines     = ctx.debug_draw != nullptr && !ctx.debug_draw->empty();
    // A barrier may name the task and mesh stages only on a device that has
    // them, so passes with a mesh and a vertex path declare by this.
    decl_ctx.mesh_shader_supported = ctx.device != nullptr && ctx.device->caps().mesh_shaders;
    // Whether this run builds the bloom, for the post pass that reads it:
    // bloom is on and this viewport's graph runs the chain to its last
    // upsample. A graph without the chain would otherwise be handed a
    // bloom_mip0 nothing wrote, whatever the recycled image last held.
    {
        const auto* bloom = passes.try_get<BloomCore>("bloom_ds_0");
        const bool runs_chain = viewport_graph == nullptr
            ? state_for("bloom_us_0").enabled
            : viewport_enabled.contains(fg_name_hash("bloom_us_0"));
        decl_ctx.bloom_enabled = bloom != nullptr && bloom->enabled() && runs_chain;
    }
    // Whether this run builds the fog volume, for passes outside the fog that
    // sample it: the fog is on and this viewport's graph integrates it.
    {
        const auto* fog = passes.try_get<FogCore>("fog_medium");
        const bool integrates = viewport_graph == nullptr
            ? state_for("fog_integrate").enabled
            : viewport_enabled.contains(fg_name_hash("fog_integrate"));
        decl_ctx.volumetric_fog_enabled = fog != nullptr && fog->enabled() && integrates;
    }

    const bool has_msaa = (msaa != gpu::Samples::x1);

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
        auto add_import = [&](std::string_view name, gpu::Texture texture) {
            decl_ctx.imports.emplace(fg_name_hash(name), ImportedImage{.view = texture});
        };
        add_import("color",        target.color_texture());
        add_import("depth",        target.depth_texture());
        // The water's own depth attachment, multisampled with the target, is
        // "msaa_surface_depth"; like the other msaa_* names it is the same image
        // as the resolved one without MSAA, and the scene's where no water draws.
        if (water_draws) {
            add_import("surface_depth",      target.surface_depth_texture());
            add_import("msaa_surface_depth", target.water_depth_texture());
        } else {
            add_import("surface_depth",      target.depth_texture());
            add_import("msaa_surface_depth", target.msaa_depth_texture());
        }
        add_import("mr",           target.mr_texture());
        add_import("normal",       target.normal_texture());
        add_import("velocity",     target.velocity_texture());
        add_import("screen_color", target.screen_color_texture());
        add_import("distortion",   target.distortion_texture());
        if (has_msaa) {
            add_import("msaa_color",    target.msaa_color_texture());
            add_import("msaa_depth",    target.msaa_depth_texture());
            add_import("msaa_mr",       target.msaa_mr_texture());
            add_import("msaa_normal",   target.msaa_normal_texture());
            add_import("msaa_velocity", target.msaa_velocity_texture());
        } else {
            // MSAA off: the msaa_* slots alias the non-MSAA images so
            // passes that unconditionally import by the msaa name still
            // resolve to a valid texture.
            add_import("msaa_color",    target.color_texture());
            add_import("msaa_depth",    target.depth_texture());
            add_import("msaa_mr",       target.mr_texture());
            add_import("msaa_normal",   target.normal_texture());
            add_import("msaa_velocity", target.velocity_texture());
        }

        if (pp_resources != nullptr) {
        if (auto* hiz = pp_resources->hiz()) {
            if (hiz->ready()) {
                decl_ctx.export_texture("hiz_pyramid", hiz->pyramid, {.persistent = true});
            }
        }
        // AO output image is per-viewport: look up the active pp_resources
        // AO here so declare-time imports match whichever viewport is
        // being recorded. AoPass::collect_exports() can't see
        // pp_resources, and default_pp_'s AO isn't the one the
        // preview viewport writes — importing the wrong one lets the
        // graph tracker's layout drift from actual image state.
        if (auto* gtao = pp_resources->gtao()) {
            if (gtao->ao) {
                ImportedImage img{};
                img.view = gtao->ao;
                img.persistent = true;
                // Sampled by the blur in compute, and by every fragment
                // shader through the global set's "ao_texture": a GTAO
                // run on the compute queue reaches those after the
                // semaphore the graphics queue waits on, so compute is
                // where it rests. Made ready to be sampled when created.
                img.resting = gpu::Access::sampled_compute;
                decl_ctx.imports.emplace(fg_name_hash("gtao_ao"), img);
            }
        }
        // Fog scatter ping-pong volumes: same per-viewport-import pattern
        // as gtao_ao. The pass advances ping_pong after composite, so
        // cur_index() here points at the volume that inject/temporal/
        // integrate all target this frame.
        if (auto* fog = pp_resources->volumetric_fog()) {
            if (fog->scatter_volumes[0]) {
                auto add_scatter = [&](std::string_view name, uint32_t idx) {
                    ImportedImage img{};
                    img.view = fog->scatter_volumes[idx];
                    img.persistent = true;
                    decl_ctx.imports.emplace(fg_name_hash(name), img);
                };
                add_scatter("fog_scatter_cur", fog->cur_index());
                add_scatter("fog_scatter_prev", fog->prev_index());
            }
        }
        // The GI outputs, under their producers' names. Both rest where the
        // surfaces sample them through the global set, in the fragment
        // shader; neither holds anything before its pass first runs.
        if (auto* ddgi = pp_resources->ddgi(); ddgi != nullptr && ddgi->indirect) {
            decl_ctx.export_texture(DdgiPass::OUTPUT_NAME, ddgi->indirect,
                                    {.persistent = true,   // the opaque pass reads it the next frame
                                     .resting = gpu::Access::sampled_fragment,
                                     .unwritten = !ddgi->indirect_written});
        }
        // DDGI's probe field, which it keeps from frame to frame and RT
        // reflections light their hits from. Made cleared, the atlases rest
        // where compute shaders and the editor's view of them sample them.
        if (auto* ddgi = pp_resources->ddgi(); ddgi != nullptr && ddgi->irradiance) {
            const TextureExport field{.persistent = true, .resting = DdgiPass::ATLAS_REST};
            decl_ctx.export_texture(DdgiPass::IRRADIANCE_NAME, ddgi->irradiance, field);
            decl_ctx.export_texture(DdgiPass::VISIBILITY_NAME, ddgi->visibility, field);
            decl_ctx.export_texture(DdgiPass::PROBE_OFFSET_NAME, ddgi->probe_offset, field);
            decl_ctx.buffer_imports.emplace(fg_name_hash(DdgiPass::PROBE_STATUS_NAME),
                                            BufferImport{.buffer = ddgi->probe_status, .persistent = true});
        }
        if (auto* ss_gi = pp_resources->ss_gi(); ss_gi != nullptr && ss_gi->indirect) {
            decl_ctx.export_texture(SsGiPass::OUTPUT_NAME, ss_gi->indirect,
                                    {.persistent = true,   // the opaque pass reads it the next frame
                                     .resting = gpu::Access::sampled_fragment,
                                     .unwritten = !ss_gi->written});
            // The depth this viewport drew last frame, which SS-GI's resolve
            // reads before its depth history pass copies this frame's over
            // it. Made for the target it draws into, which a resize or
            // another viewport sharing these images can change.
            if (const auto* pass = passes.try_get<SsGiPass>("ss_gi"); pass != nullptr && pass->active()) {
                ss_gi->fit_prev_depth(*ctx.device, target.depth_texture());
                if (ss_gi->prev_depth) {
                    decl_ctx.export_texture(SsGiPass::PREV_DEPTH_NAME, ss_gi->prev_depth,
                                            {.persistent = true});
                }
            }
        }
        // The ray-traced reflection the SSR composite reads. Made cleared,
        // which leaves it where the composite samples it: a frame the trace
        // skips reads what the last one left, never an undefined image.
        if (auto* rt = pp_resources->rt_reflection(); rt != nullptr && rt->output) {
            decl_ctx.export_texture(RtReflectionPass::OUTPUT_NAME, rt->output,
                                    {.resting = gpu::Access::sampled_compute});
        }
        // TAA's history ping-pong: the slot it writes this frame, which the
        // post pass then reads, and the one it reads back from the frame
        // before. Persistent: each slot is next frame's read.
        if (auto* taa = pp_resources->taa();
            taa != nullptr && taa->history[0] && taa->history[1]) {
            auto add_history_slot = [&](std::string_view name, uint32_t idx) {
                decl_ctx.export_texture(name, taa->history[idx], {.persistent = true});
            };
            add_history_slot("taa_history_write", taa->write_index);
            add_history_slot("taa_history_read", 1 - taa->write_index);
        }
        // The post pass's output, what the viewport shows: written as a colour
        // attachment, and resting where ImGui samples it after the graph.
        if (auto* output = pp_resources->post(); output != nullptr && output->texture) {
            ImportedImage img{};
            img.view = output->texture;
            img.resting = gpu::Access::sampled_fragment;
            img.unwritten = !output->written;
            decl_ctx.imports.emplace(fg_name_hash("post_output"), img);
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
            auto& imgs = shafts->frame_images(decl_ctx.viewport_extent);
            auto add_history = [&](std::string_view name, uint32_t idx) {
                decl_ctx.export_texture(name, imgs.shafts[idx], {.persistent = true});
            };
            add_history(SunShaftsPass::HISTORY_CUR_NAME, imgs.cur_index());
            add_history(SunShaftsPass::HISTORY_PREV_NAME, imgs.prev_index());
        }

        passes.collect_exports_all(decl_ctx);
        for (uint32_t i = 0; i < ctx.subsystem_count; ++i) {
            ctx.subsystems[i]->collect_exports(decl_ctx);
        }

        // The names the surfaces read the GI outputs by stand for what the
        // global set binds at those slots: the output once it holds
        // something, nothing while the set binds the fallback.
        auto name_if_bound = [&](std::string_view slot, std::string_view output, gpu::Texture bound) {
            const auto it = decl_ctx.imports.find(fg_name_hash(output));
            if (!bound.valid() || it == decl_ctx.imports.end() || it->second.view.texture != bound) {
                return;
            }
            const ImportedImage img = it->second;
            decl_ctx.imports.emplace(fg_name_hash(slot), img);
        };
        name_if_bound("gi_indirect", DdgiPass::OUTPUT_NAME, ctx.bound_gi_indirect);
        name_if_bound("ss_gi_emissive", SsGiPass::OUTPUT_NAME, ctx.bound_ss_gi_emissive);
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
                return {.texture = imp.view.texture,
                        .base_layer = imp.view.base_layer,
                        .layer_count = imp.view.layer_count};
            }
        }
        for (const auto& cre : b.created_textures()) {
            if (cre.handle.id == h.id) {
                return {.virtual_name_hash = cre.name_hash};
            }
        }
        return {};
    };

    // Buffer keys: imported buffers identify by their handle (offset/range
    // is sub-resource detail the graph doesn't model yet — coarse buffer
    // identity is enough for producer/consumer edges), and imported
    // acceleration structures by theirs. Created (transient) buffers key on
    // FNV-1a hash of name, mirroring the texture path.
    auto resolve_buffer_key = [](const PassBuilder& b, FgBuffer h) -> ResKey {
        for (const auto& imp : b.imported_buffers()) {
            if (imp.handle.id == h.id) {
                return {.buffer = imp.buffer, .structure = imp.structure};
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
        return k.texture.valid() || k.buffer.valid() || k.structure.valid() ||
               k.virtual_name_hash != 0;
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
        auto record_access = [&](size_t j, const ResKey& k, gpu::Access access) {
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
    graph.begin_frame(*ctx.device);
    auto color_id        = graph.register_image(target.color_texture());
    auto msaa_color_id   = has_msaa ? graph.register_image(target.msaa_color_texture()) : color_id;
    auto depth_id        = graph.register_image(target.depth_texture());
    auto msaa_depth_id   = has_msaa ? graph.register_image(target.msaa_depth_texture()) : depth_id;
    auto surface_depth_id = water_draws ? graph.register_image(target.surface_depth_texture()) : depth_id;
    auto mr_id           = graph.register_image(target.mr_texture());
    auto msaa_mr_id      = has_msaa ? graph.register_image(target.msaa_mr_texture()) : mr_id;
    auto normal_id       = graph.register_image(target.normal_texture());
    auto msaa_normal_id  = has_msaa ? graph.register_image(target.msaa_normal_texture()) : normal_id;
    auto velocity_id     = graph.register_image(target.velocity_texture());
    auto msaa_velocity_id = has_msaa ? graph.register_image(target.msaa_velocity_texture()) : velocity_id;
    auto screen_color_id = graph.register_image(target.screen_color_texture());
    auto distortion_id   = graph.register_image(target.distortion_texture());

    registry.register_image("color",         {.frame_graph_id = color_id,         .texture = target.color_texture()});
    registry.register_image("msaa_color",    {.frame_graph_id = msaa_color_id,    .texture = target.msaa_color_texture()});
    registry.register_image("depth",         {.frame_graph_id = depth_id,         .texture = target.depth_texture()});
    registry.register_image("msaa_depth",    {.frame_graph_id = msaa_depth_id,    .texture = target.msaa_depth_texture()});
    registry.register_image("surface_depth", {.frame_graph_id = surface_depth_id,
                                              .texture = water_draws ? target.surface_depth_texture() : target.depth_texture()});
    registry.register_image("mr",            {.frame_graph_id = mr_id,            .texture = target.mr_texture()});
    registry.register_image("msaa_mr",       {.frame_graph_id = msaa_mr_id,       .texture = target.msaa_mr_texture()});
    registry.register_image("normal",        {.frame_graph_id = normal_id,        .texture = target.normal_texture()});
    registry.register_image("msaa_normal",   {.frame_graph_id = msaa_normal_id,   .texture = target.msaa_normal_texture()});
    registry.register_image("velocity",      {.frame_graph_id = velocity_id,      .texture = target.velocity_texture()});
    registry.register_image("msaa_velocity", {.frame_graph_id = msaa_velocity_id, .texture = target.msaa_velocity_texture()});
    registry.register_image("screen_color",  {.frame_graph_id = screen_color_id,  .texture = target.screen_color_texture()});
    registry.register_image("distortion",    {.frame_graph_id = distortion_id,    .texture = target.distortion_texture()});

    for (size_t idx : dag_order) {
        if (!alive[idx]) { continue; }
        auto& pp = pending[idx];
        graph.submit_declared_pass(
            entries_[pp.entry_index].name,
            builders_[pp.builder],
            [p = pp.pass, &ctx](VkCommandBuffer cb) {
                FrameContext local = ctx;
                local.cmd = cb;
                const gpu::Queue queue = cb == ctx.async_compute_cmd ? gpu::Queue::compute
                                                                      : gpu::Queue::graphics;
                gpu::vulkan::CommandBufferList commands(*ctx.device, cb, queue);
                local.command_list = &commands.list();
                p->record(local);
            });
    }
    } // rp_graph_submit

    // Take a texture per alias group from the cross-frame pool and
    // register each logical resource under the name it was declared
    // with, where passes read it through ctx.texture(name).
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
            gpu::TextureUses uses{};
            for (uint32_t rid : group.resource_ids) {
                uses |= lifetimes[rid].uses;
            }
            const gpu::Texture texture =
                ctx.transient_pool->acquire(group.desc, decl_ctx.viewport_extent, uses);
            if (!texture.valid()) { continue; }
            for (uint32_t rid : group.resource_ids) {
                const auto& img = imgs[rid];
                if (img.name.empty()) { continue; }
                ResourceRegistry::ImageResource res{};
                res.frame_graph_id = rid;
                res.texture = texture;
                res.mip_count = img.mip_count;
                registry.register_image(img.name, res);
                // Tell the graph which texture backs this virtual resource
                // this frame, so emit_barriers_for_pass has something to
                // transition. Slice state is already seeded to UNDEFINED
                // by begin_frame(), which is what we want — the first
                // access will barrier out of UNDEFINED regardless of
                // whether the pool handed back a fresh or reused image.
                graph.bind_virtual_image(rid, texture);
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
