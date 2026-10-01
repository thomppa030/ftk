#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/render_pipeline.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

/// Type-erased storage for render passes. Passes are registered by name
/// and looked up by concrete type via get<T>(). Owns all pass instances.
///
/// Also drives pipeline registration: register_with_pipeline() iterates
/// all passes, extracts their setup() metadata (name, phase, sub_order),
/// and hands the RenderPipeline a PassEntry pointing at the pass so it
/// can invoke declare()/record() directly.
class PassRegistry {
public:
    /// Register a pass by name. Takes ownership.
    template<typename T>
    T& add(const std::string& name, std::unique_ptr<T> pass) {
        auto* ptr = pass.get();
        order_.push_back(name);
        passes_[name] = std::move(pass);
        return *ptr;
    }

    /// Look up a pass by name, cast to concrete type. Asserts if not found.
    template<typename T>
    [[nodiscard]] T& get(const std::string& name) {
        auto it = passes_.find(name);
        assert(it != passes_.end() && "PassRegistry::get: pass not found");
        return static_cast<T&>(*it->second);
    }

    /// Look up a pass by name, cast to concrete type. Returns nullptr if not found.
    template<typename T>
    [[nodiscard]] T* try_get(const std::string& name) {
        auto it = passes_.find(name);
        if (it == passes_.end()) return nullptr;
        return static_cast<T*>(it->second.get());
    }

    /// Destroy all passes (in unspecified order).
    void destroy_all() {
        for (auto& [name, pass] : passes_) {
            pass->destroy();
        }
        passes_.clear();
        order_.clear();
    }

    /// Check if a pass is registered.
    [[nodiscard]] bool has(const std::string& name) const {
        return passes_.contains(name);
    }

    /// Run post_create() on all passes in insertion order.
    void post_create_all(GpuContext& gpu) {
        for (const auto& name : order_) {
            passes_[name]->post_create(gpu, *this);
        }
    }

    /// Run prepare_frame() on all passes (once per frame, before any viewport).
    void prepare_frame_all(const FrameContext& ctx, GpuContext& gpu) {
        for (const auto& name : order_) {
            passes_[name]->prepare_frame(ctx, gpu);
        }
    }

    /// Run pre_record() on all passes in insertion order.
    void pre_record_all(const FrameContext& ctx, GpuContext& gpu) {
        for (const auto& name : order_) {
            passes_[name]->pre_record(ctx, gpu);
        }
    }

    /// Run post_record() on all passes in insertion order.
    void post_record_all(const FrameContext& ctx) {
        for (const auto& name : order_) {
            passes_[name]->post_record(ctx);
        }
    }

    /// Let all passes register their persistent resources.
    void register_resources_all(ftk::ResourceRegistry& registry) {
        for (const auto& name : order_) {
            passes_[name]->register_resources(registry);
        }
    }

    /// Gather every pass's persistent exports into the DeclareContext's
    /// named-image catalog. Called by RenderPipeline before any pass's
    /// declare() runs, so consumers can resolve "shadow_atlas" etc.
    /// through PassBuilder::import_named().
    void collect_exports_all(DeclareContext& ctx) {
        for (const auto& name : order_) {
            passes_[name]->collect_exports(ctx);
        }
    }

    /// Register all passes with a RenderPipeline. Iterates passes in
    /// insertion order, extracts setup() metadata, and creates one
    /// PassEntry per pass.
    void register_with_pipeline(RenderPipeline& pipeline);

    /// Register a single pass (by registry name) with the pipeline.
    /// Used for passes created after initial registration (e.g. lazy meshlet init).
    void register_pass_with_pipeline(const std::string& name,
                                     RenderPipeline& pipeline);

private:
    std::unordered_map<std::string, std::unique_ptr<RenderPass>> passes_;
    std::vector<std::string> order_; // insertion order for deterministic iteration
};

} // namespace fjell
