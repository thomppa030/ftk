#pragma once

#include "ftk/base/handle.hpp"

#include <cstdint>

namespace fjell {

using ModelHandle         = ftk::Handle<struct ModelTag>;
using MaterialHandle      = ftk::Handle<struct MaterialTag>;
using TextureHandle       = ftk::Handle<struct TextureTag>;
using SkeletalModelHandle = ftk::Handle<struct SkeletalModelTag>;
using SkeletonHandle      = ftk::Handle<struct SkeletonTag>;
using AnimationClipHandle = ftk::Handle<struct AnimationClipTag>;
using VfxAssetHandle      = ftk::Handle<struct VfxAssetTag>;
using AudioAssetHandle    = ftk::Handle<struct AudioAssetTag>;
using InputActionHandle   = ftk::Handle<struct InputActionTag>;
using PresetAssetHandle   = ftk::Handle<struct PresetAssetTag>;
using TerrainAssetHandle  = ftk::Handle<struct TerrainAssetTag>;
using SurfaceAssetHandle  = ftk::Handle<struct SurfaceAssetTag>;
using DayProfileAssetHandle = ftk::Handle<struct DayProfileAssetTag>;
using WeatherAssetHandle  = ftk::Handle<struct WeatherAssetTag>;
using ClimateAssetHandle  = ftk::Handle<struct ClimateAssetTag>;

enum class LoadState : uint8_t {
    Loading,
    Ready,
    Failed,
};

} // namespace fjell
