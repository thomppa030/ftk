#pragma once

#include <cstdint>

namespace fjell {

/// Generational resource handle.
/// Bits [0..19]  = slot index  (max 1M entries)
/// Bits [20..31] = generation  (wraps after 4096)
template <typename Tag>
struct Handle {
    uint32_t id{0};

    [[nodiscard]] bool valid() const { return id != 0; }
    [[nodiscard]] uint32_t index() const { return id & 0xFFFFF; }
    [[nodiscard]] uint32_t generation() const { return id >> 20; }

    static Handle make(uint32_t index, uint32_t gen) {
        return {(gen << 20) | (index & 0xFFFFF)};
    }

    bool operator==(const Handle&) const = default;
};

using ModelHandle         = Handle<struct ModelTag>;
using MaterialHandle      = Handle<struct MaterialTag>;
using TextureHandle       = Handle<struct TextureTag>;
using SkeletalModelHandle = Handle<struct SkeletalModelTag>;
using SkeletonHandle      = Handle<struct SkeletonTag>;
using AnimationClipHandle = Handle<struct AnimationClipTag>;
using VfxAssetHandle      = Handle<struct VfxAssetTag>;
using AudioAssetHandle    = Handle<struct AudioAssetTag>;
using InputActionHandle   = Handle<struct InputActionTag>;
using PresetAssetHandle   = Handle<struct PresetAssetTag>;

enum class LoadState : uint8_t {
    Loading,
    Ready,
    Failed,
};

} // namespace fjell
