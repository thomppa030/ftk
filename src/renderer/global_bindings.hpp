#pragma once

#include "gpu/binding.hpp"
#include "gpu/owned.hpp"
#include "gpu/shader.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
}

/// The global set every pass may bind at set 0: the frame's uniforms and
/// lights, the shadow maps, the sky, and the view's depth, AO and GI. Its
/// layout is made by the device from the bindings declared here; shaders
/// declare them by number.
///
/// Each frame slot binds a group of its own, and each offscreen view
/// (scene viewport, camera preview, thumbnails) one it owns, refreshed from
/// the slot it renders in with its own view-dependent images, so writing one
/// never touches a set a pending frame reads. What is set is kept per slot:
/// a slot's group is made from it the first time it is asked for, when every
/// pass has set its binding.
class GlobalBindings {
public:
    /// Declares the built-in bindings.
    GlobalBindings();

    /// Makes the layout. `gpu` is what the groups are made and written with.
    /// @return nothing, or why the layout could not be made.
    [[nodiscard]] Result<> create(gpu::Device& gpu);

    /// The layout pipelines name to bind the set.
    [[nodiscard]] gpu::SharedLayout layout() const { return layout_; }

    /// The group the frames of `slot` bind, made the first time it is asked
    /// for. Empty, and reported once, while a binding has never been set.
    [[nodiscard]] gpu::BindGroup group(uint32_t slot);

    /// Points the binding `name` at `resource` in every slot's group.
    void set(std::string_view name, const gpu::BindResource& resource);

    /// `set()` for one slot's group only: for a binding that changes with
    /// the view drawn, set for the slot about to record.
    void set(uint32_t slot, std::string_view name, const gpu::BindResource& resource);

    /// Several bindings of one slot's group at once, as one new version of
    /// it: a view's run points the bindings it fills at its own.
    void set(uint32_t slot, std::span<const gpu::BindEntry> entries);

    /// A group of the set for an offscreen view to own; `refresh_view()`
    /// brings it up to date before each render. Empty, and reported once,
    /// while a binding has never been set.
    [[nodiscard]] gpu::Owned<gpu::BindGroup> make_view_group(std::string_view name);

    /// Writes into `view` what `slot` holds, with `overrides` (the view's
    /// own depth, AO and the like) in place of the slot's.
    void refresh_view(gpu::BindGroup view, uint32_t slot, std::span<const gpu::BindEntry> overrides);

    /// Releases the groups. The layout lasts as long as the device.
    void destroy();

private:
    struct Declared {
        std::string name;
        uint32_t binding{0};
        gpu::BindingKind kind{gpu::BindingKind::sampled_texture};
        gpu::ShaderStages stages{};
    };

    void declare(Declared decl);
    /// Every binding's entry as `slot` holds it, with `overrides` in place of
    /// the slot's; none when one was never set, which is reported once by
    /// name.
    [[nodiscard]] std::optional<std::vector<gpu::BindEntry>> entries(
        uint32_t slot, std::span<const gpu::BindEntry> overrides = {});

    gpu::Device* gpu_{nullptr};
    gpu::SharedLayout layout_{};
    std::vector<Declared> declared_;
    struct NameHash {
        using is_transparent = void;
        size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
    };
    std::unordered_map<std::string, size_t, NameHash, std::equal_to<>> index_;
    /// What each slot holds, by declared binding.
    std::array<std::vector<std::optional<gpu::BindResource>>, MAX_FRAMES_IN_FLIGHT> contents_;
    std::array<gpu::Owned<gpu::BindGroup>, MAX_FRAMES_IN_FLIGHT> groups_;
    bool missing_reported_{false};
};

} // namespace fjell
