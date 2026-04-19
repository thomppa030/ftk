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
                              VkSampleCountFlagBits msaa, uint32_t /*active_sources*/,
                              ThreadPool& pool, ThreadCommandPools& secondary_pools,
                              uint32_t frame_index, ResourceRegistry& registry,
                              PassRegistry& passes, PostProcessResources* pp_resources) {
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

    // ── Phase 2 commit #1: shadow DAG sort ────────────────────────────
    // Run a declare-driven topo sort in parallel with the legacy path and
    // log (once) whether the resulting pass order matches the current
    // phase+sub_order ordering. No behavior change: the legacy build()
    // loop below is still the one that actually executes.
    if (!dag_order_logged_) {
        DeclareContext decl_ctx;
        decl_ctx.viewport_extent = {target.extent().width, target.extent().height};
        decl_ctx.msaa_samples    = msaa;
        decl_ctx.frame_index     = frame_index;
        // Scene-summary + feature toggles stay at defaults in C1. No Phase 1
        // port branches on them, so topology is identical regardless.

        // Populate the named-image catalog with the viewport's framebuffer
        // images. Consumers look these up by name through
        // PassBuilder::import_named(). Producer passes add their persistent
        // outputs (shadow atlases, Hi-Z, cubemaps) via collect_exports_all.
        const bool has_msaa_decl = (msaa != VK_SAMPLE_COUNT_1_BIT);
        auto add_import = [&](std::string name, VkImage img, VkImageView view,
                               VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
            decl_ctx.imports.emplace(std::move(name),
                ImportedImage{img, view, aspect, 0, 1});
        };
        add_import("color",        target.color_image(),        target.color_view());
        add_import("depth",        target.depth_image(),        target.depth_view(),
                   VK_IMAGE_ASPECT_DEPTH_BIT);
        add_import("mr",           target.mr_image(),           target.mr_view());
        add_import("normal",       target.normal_image(),       target.normal_view());
        add_import("velocity",     target.velocity_image(),     target.velocity_view());
        add_import("screen_color", target.screen_color_image(), target.screen_color_view());
        add_import("distortion",   target.distortion_image(),   target.distortion_view());
        if (has_msaa_decl) {
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

        // Viewport-scoped exports: Hi-Z pyramid lives on PostProcessResources,
        // not on any RenderPass, so the pipeline seeds it here. Per-viewport
        // resources that several passes may consume go through the same path.
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

        struct DeclaredPass {
            size_t entry_index{0};
            PassBuilder builder;
        };
        std::vector<DeclaredPass> declared;
        declared.reserve(entries_.size());

        for (size_t i = 0; i < entries_.size(); ++i) {
            auto* p = entries_[i].pass;
            if (p == nullptr || !p->uses_declare()) continue;
            if (!state_for(entries_[i].name).enabled) continue;
            DeclaredPass d;
            d.entry_index = i;
            p->declare(d.builder, decl_ctx);
            declared.push_back(std::move(d));
        }

        // Resource identity: (VkImage, base_layer, layer_count). Builder-
        // local created handles aren't represented yet (no pass in Phase 1
        // uses create()) — they'll surface in Phase 3.
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

        const size_t n = declared.size();
        std::vector<std::vector<size_t>> adj(n);
        std::vector<uint32_t> in_degree(n, 0);

        // Map texture-handle id -> the resource key the pass imported it
        // under, scoped per-pass (builder-local handle ids).
        auto resolve_key = [](const PassBuilder& b, FgTexture h) -> ResKey {
            for (const auto& imp : b.imported_textures()) {
                if (imp.handle.id == h.id) {
                    return {imp.image, imp.base_layer, imp.layer_count};
                }
            }
            return {VK_NULL_HANDLE, 0, 0};
        };

        std::unordered_map<ResKey, size_t, ResKeyHash> last_writer;

        // access_is_write / access_is_read from resource_desc.hpp cover
        // the buffer side too; for the texture loop both sides of a
        // read_write access should trigger the WAR edge AND update the
        // last writer, so we mirror that behaviour here.

        for (size_t j = 0; j < n; ++j) {
            const auto& b = declared[j].builder;
            for (const auto& acc : b.texture_accesses()) {
                ResKey k = resolve_key(b, acc.handle);
                if (k.image == VK_NULL_HANDLE) continue;
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

        // Kahn's algorithm; tiebreaker = original entry index (==
        // current phase+sub_order after the legacy sort above).
        auto cmp = [&declared](size_t a, size_t b) {
            return declared[a].entry_index > declared[b].entry_index;
        };
        std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);
        for (size_t i = 0; i < n; ++i) {
            if (in_degree[i] == 0) pq.push(i);
        }
        std::vector<size_t> dag_order;
        dag_order.reserve(n);
        while (!pq.empty()) {
            size_t idx = pq.top();
            pq.pop();
            dag_order.push_back(idx);
            for (size_t next : adj[idx]) {
                if (--in_degree[next] == 0) pq.push(next);
            }
        }

        const bool cycle_detected = (dag_order.size() != n);

        // Compare against the legacy order restricted to enabled,
        // uses_declare() passes (== the input to the DAG sort).
        bool matches = !cycle_detected;
        if (matches) {
            for (size_t i = 0; i < n; ++i) {
                if (declared[dag_order[i]].entry_index != declared[i].entry_index) {
                    matches = false;
                    break;
                }
            }
        }

        if (cycle_detected) {
            FJELL_GFX_WARN(
                "DAG shadow sort: cycle detected ({} of {} passes scheduled). "
                "Legacy path still in control — investigate declare() edges.",
                static_cast<unsigned>(dag_order.size()), static_cast<unsigned>(n));
        } else if (matches) {
            FJELL_GFX_INFO("DAG shadow sort: {} passes, order matches legacy.",
                           static_cast<unsigned>(n));
        } else {
            std::ostringstream legacy_ss;
            std::ostringstream dag_ss;
            for (size_t i = 0; i < n; ++i) {
                if (i != 0) {
                    legacy_ss << ", ";
                    dag_ss << ", ";
                }
                legacy_ss << entries_[declared[i].entry_index].name;
                dag_ss << entries_[declared[dag_order[i]].entry_index].name;
            }
            std::string legacy_str = legacy_ss.str();
            std::string dag_str = dag_ss.str();
            FJELL_GFX_WARN(
                "DAG shadow sort: {} passes, order differs from legacy.\n"
                "  legacy: {}\n"
                "  dag:    {}",
                static_cast<unsigned>(n), legacy_str.c_str(), dag_str.c_str());
        }

        dag_order_logged_ = true;
    }

    // Register images with the frame graph and populate the resource registry
    FrameGraph graph;
    bool has_msaa = (msaa != VK_SAMPLE_COUNT_1_BIT);

    auto color_id = graph.register_image(target.color_image());
    auto msaa_color_id = color_id;
    if (has_msaa) {
        msaa_color_id = graph.register_image(target.msaa_color_image());
    }

    auto depth_id = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    auto msaa_depth_id = depth_id;
    if (has_msaa) {
        msaa_depth_id = graph.register_image(target.msaa_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    auto mr_id = graph.register_image(target.mr_image());
    auto msaa_mr_id = mr_id;
    if (has_msaa) {
        msaa_mr_id = graph.register_image(target.msaa_mr_image());
    }

    auto normal_id = graph.register_image(target.normal_image());
    auto msaa_normal_id = normal_id;
    if (has_msaa) {
        msaa_normal_id = graph.register_image(target.msaa_normal_image());
    }

    auto velocity_id = graph.register_image(target.velocity_image());
    auto msaa_velocity_id = velocity_id;
    if (has_msaa) {
        msaa_velocity_id = graph.register_image(target.msaa_velocity_image());
    }

    auto screen_color_id = graph.register_image(target.screen_color_image());
    auto distortion_id = graph.register_image(target.distortion_image());

    graph.begin_frame();

    // Populate resource registry (bridge string names to FrameGraph IDs + views)
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

    // Each enabled entry adds itself to the graph with its image dependencies
    for (auto& entry : entries_) {
        if (!state_for(entry.name).enabled) continue;
        entry.build(graph, registry);
    }

    graph.execute(cmd, &pool, &secondary_pools, frame_index);
}

} // namespace fjell
