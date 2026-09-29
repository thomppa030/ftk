#pragma once

#include "gpu/binding.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
}

/// Declaration for a single global descriptor set binding.
struct GlobalBindingDecl {
    std::string name;
    uint32_t binding;           ///< Explicit number, or BINDING_AUTO for auto-assignment
    VkDescriptorType type;
    uint32_t count{1};
    VkShaderStageFlags stages;
};

/// Owns the global descriptor set layout, pool, and per-frame sets.
///
/// Two-phase lifecycle:
///   1. **Registration** — `declare()` bindings before layout creation.
///      Constructor registers the 11 builtin bindings (0-10).
///   2. **Finalized** — after `finalize()`, layout/pool/sets are created.
///      Lookup and write helpers become available; `declare()` is illegal.
class GlobalBindings {
public:
    static constexpr uint32_t BINDING_AUTO = ~0u;

    GlobalBindings();

    // ── Phase 1: Registration ──────────────────────────────────────────

    /// Declare a binding. Must be called before finalize().
    void declare(GlobalBindingDecl decl);

    // ── Phase 1 → 2 transition ─────────────────────────────────────────

    /// Build layout, pool, and allocate per-frame descriptor sets. `gpu` is
    /// what `set()` resolves the interface's handles through.
    void finalize(VkDevice device, uint32_t frames_in_flight, gpu::Device& gpu);

    // ── Phase 2: Lookup ────────────────────────────────────────────────

    [[nodiscard]] VkDescriptorSetLayout layout() const {
        assert(finalized_ && "GlobalBindings::layout() called before finalize()");
        return layout_->handle();
    }

    [[nodiscard]] VkDescriptorSet descriptor_set(uint32_t frame_index) const {
        assert(finalized_ && "GlobalBindings::descriptor_set() called before finalize()");
        return descriptor_sets_[frame_index];
    }

    /// The bindings the layout was made from, for the GPU interface's shared
    /// layout.
    [[nodiscard]] std::span<const VkDescriptorSetLayoutBinding> layout_bindings() const {
        return layout_bindings_;
    }
    [[nodiscard]] uint32_t binding_number(const std::string& name) const;
    [[nodiscard]] bool has_binding(const std::string& name) const;

    // ── Auxiliary sets (previews) ──────────────────────────────────────
    //
    // Offscreen viewports (camera preview, SceneViewport) must not write the
    // shared per-frame sets the main command buffer binds — doing so while a
    // prior frame's CB is still pending is an unsynchronized descriptor update
    // (DEVICE_LOST). They own an auxiliary set instead: copy the per-frame
    // set's static bindings, then overwrite the view-dependent images.

    /// Allocate a private global set (same layout). Free with free_set().
    [[nodiscard]] VkDescriptorSet allocate_set();

    /// Return an auxiliary set to the pool. Caller guarantees no in-flight CB
    /// still binds it (previews call vk.wait_idle() before teardown).
    void free_set(VkDescriptorSet set);

    /// Copy every binding from the per-frame set into `dst` (one vkUpdate).
    /// Source must be idle — call after that frame slot's fence has signaled.
    void copy_from_frame(VkDescriptorSet dst, uint32_t src_frame_index);

    // ── Phase 2: Descriptor writes ─────────────────────────────────────

    /// Stage an image descriptor write. Call flush_writes() to submit.
    void write_image(uint32_t frame_index, const std::string& name,
                     VkImageView view, VkSampler sampler,
                     VkImageLayout image_layout);

    /// Stage a buffer descriptor write. Call flush_writes() to submit.
    void write_buffer(uint32_t frame_index, const std::string& name,
                      VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);

    /// Stage an image write targeting an explicit set (e.g. a preview's
    /// auxiliary set from allocate_set()) instead of a per-frame set.
    void write_image_to(VkDescriptorSet set, const std::string& name,
                        VkImageView view, VkSampler sampler,
                        VkImageLayout image_layout);

    /// Submit all staged writes via vkUpdateDescriptorSets, then clear.
    void flush_writes();

    /// Points the binding `name` at `resource` in every per-frame set, written
    /// at once: a texture read through a sampler, in the layout sampling
    /// reads, or as storage, or a buffer range. For what a pass makes
    /// through the GPU interface.
    void set(const std::string& name, const gpu::BindResource& resource);

    /// `set()` for one frame slot's set only: for a binding that changes
    /// every frame, written for the slot about to record, whose last frame
    /// the GPU has finished.
    void set(uint32_t frame_index, const std::string& name, const gpu::BindResource& resource);

    // ── Cleanup ────────────────────────────────────────────────────────

    void destroy();

private:
    void register_builtins();

    struct BindingEntry {
        GlobalBindingDecl decl;
    };

    bool finalized_{false};
    VkDevice device_{VK_NULL_HANDLE};
    gpu::Device* gpu_{nullptr};

    // Registration data
    std::vector<BindingEntry> entries_;
    std::unordered_map<std::string, uint32_t> name_to_binding_;
    uint32_t next_auto_binding_{0};

    // Vulkan objects (created by finalize)
    std::unique_ptr<DescriptorSetLayout> layout_;
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings_;
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};

    // Separate, freeable pool for auxiliary (preview) sets. Created with
    // FREE_DESCRIPTOR_SET_BIT so free_set() reclaims individual sets — the
    // main per-frame pool above is allocate-once and never freed.
    VkDescriptorPool aux_pool_{VK_NULL_HANDLE};

    // Write batching (stable addresses via deque)
    std::deque<VkDescriptorImageInfo> pending_image_infos_;
    std::deque<VkDescriptorBufferInfo> pending_buffer_infos_;
    std::vector<VkWriteDescriptorSet> pending_writes_;
};

} // namespace fjell
