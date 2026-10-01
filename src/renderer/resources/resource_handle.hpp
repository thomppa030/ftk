#pragma once

#include "ftk/base/handle.hpp"

#include <cstdint>

namespace fjell {

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
using TerrainAssetHandle  = Handle<struct TerrainAssetTag>;
using SurfaceAssetHandle  = Handle<struct SurfaceAssetTag>;
using DayProfileAssetHandle = Handle<struct DayProfileAssetTag>;
using WeatherAssetHandle  = Handle<struct WeatherAssetTag>;
using ClimateAssetHandle  = Handle<struct ClimateAssetTag>;

enum class LoadState : uint8_t {
    Loading,
    Ready,
    Failed,
};

} // namespace fjell
