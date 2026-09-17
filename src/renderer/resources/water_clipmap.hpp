#pragma once

#include "core/math/clipmap.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace fjell {

/// How many levels an infinite water surface is drawn with.
///
/// Seven, sized against a tuned six-train sea on 2026-09-17: one metre
/// between vertices on level 0, doubling per level, reaching 4224 m from the
/// camera on level 6. At eye height the horizon is about five kilometres
/// away, so that disc reaches it for any camera standing in the water. A
/// camera on a cliff can see the edge of the sea, which is the known limit of
/// having no horizon skirt rather than a defect.
inline constexpr std::uint32_t WATER_CLIPMAP_LEVELS = 7;

/// How many patches a level spans, per side.
inline constexpr std::uint32_t WATER_CLIPMAP_RING = 12;

/// How many patches of a level are left undrawn for the finer level inside it.
///
/// Five rather than the six the spans would suggest. Each level snaps to its
/// own patch grid, so level L-1 lands anywhere within one of level L's
/// patches, and a six-wide hole — exactly the inner level's own span — is
/// only covered when the two happen to align. The sixth patch is the one that
/// falls half outside, so the hole gives it up and the ring draws it: one
/// patch of overlap, which is invisible, instead of a band of sea no level
/// draws, which is a tear straight through to the sky.
///
/// Where the hole sits is not fixed, which is the other half of the same
/// point. See `WaterClipmap::hole_offset()`.
inline constexpr std::uint32_t WATER_CLIPMAP_HOLE = 5;

/// Quads along one side of a patch, matching PATCH_QUADS in water.mesh.
///
/// The number is the mesh shader's, not a choice made here: primitives bind
/// before vertices in the output limits and 2N^2 outgrows (N+1)^2, so 11
/// quads (144 verts, 242 prims) is the largest square patch fitting the
/// 256/256 floor.
inline constexpr std::uint32_t WATER_PATCH_QUADS = 11;

/// One clipmap level, as the mesh shader reads it.
///
/// Mirrors `FjellWaterClipmapLevel` in shaders/include/fjell_water_clipmap.glsl.
/// Padded to a vec4 boundary so the std140 array stride is 16 and neither
/// side has to guess at it.
struct GpuWaterClipmapLevel {
    /// World XZ of this level's minimum corner, snapped to its patch grid.
    glm::vec2 origin{0.0f};
    /// Metres between adjacent vertices on this level: 2^level.
    float cell_size{1.0f};
    /// Which patch this level's hole starts at, on both axes. Zero on level
    /// 0, which has no hole.
    std::uint32_t hole_offset{0};
};
static_assert(sizeof(GpuWaterClipmapLevel) == 16,
              "must match FjellWaterClipmapLevel in GLSL");

/// The level table, as one uniform buffer.
struct GpuWaterClipmap {
    GpuWaterClipmapLevel levels[WATER_CLIPMAP_LEVELS];
    std::uint32_t level_count{WATER_CLIPMAP_LEVELS};
    std::uint32_t ring{WATER_CLIPMAP_RING};
    std::uint32_t hole{WATER_CLIPMAP_HOLE};
    std::uint32_t patch_quads{WATER_PATCH_QUADS};
};
static_assert(sizeof(GpuWaterClipmap) == WATER_CLIPMAP_LEVELS * 16 + 16,
              "must match FjellWaterClipmap in GLSL");

/// A camera-following set of nested levels covering an infinite water
/// surface.
///
/// Each level is a `ClipmapLevel2D` over the ground plane, doubling in cell
/// size and so in coverage. Level 0 is solid; every level above it is a ring
/// with a hole left for the finer level below, which is what makes the whole
/// thing cost a ring per level rather than a square.
///
/// **Levels snap to their patch grid, not their cell grid.** A level whose
/// origin moved by one cell would shift its patch boundaries by a fraction of
/// a patch, so a ring would stop being a ring. Snapping to `cell_size *
/// WATER_PATCH_QUADS` keeps whole patches aligned across every shift, and is
/// what stops the sea crawling as the camera moves: a vertex holds its world
/// position, so it holds its place in the wave field.
class WaterClipmap {
public:
    WaterClipmap();

    /// Move the levels to follow a camera. Idempotent per level: a level does
    /// not move until the camera has crossed a full patch of that level's own
    /// size, so the outer levels sit still while the inner ones step.
    void recenter(const glm::vec3& camera_position);

    /// The table the mesh shader reads.
    [[nodiscard]] const GpuWaterClipmap& gpu() const { return gpu_; }

    /// Total patches across every level, which is the mesh task count for one
    /// draw of the whole surface. Constant: the hole moves but never changes
    /// size, so the draw does not resize with the camera.
    [[nodiscard]] static constexpr std::uint32_t patch_count() {
        const std::uint32_t full = WATER_CLIPMAP_RING * WATER_CLIPMAP_RING;
        const std::uint32_t hole = WATER_CLIPMAP_HOLE * WATER_CLIPMAP_HOLE;
        return full + (WATER_CLIPMAP_LEVELS - 1) * (full - hole);
    }

    /// Metres between vertices on the finest level, which is what the wave
    /// field's shortest train has to be measured against.
    [[nodiscard]] static constexpr float finest_spacing() { return 1.0f; }

    /// How far the surface reaches from the camera, in metres.
    [[nodiscard]] static constexpr float half_extent() {
        return static_cast<float>(WATER_PATCH_QUADS * WATER_CLIPMAP_RING *
                                  (1U << (WATER_CLIPMAP_LEVELS - 1))) *
               0.5F;
    }

private:
    std::array<ClipmapLevel2D, WATER_CLIPMAP_LEVELS> levels_{};
    GpuWaterClipmap gpu_{};
};

/// Where one patch of the clipmap sits, resolved from a flat patch index.
///
/// The mesh shader does this same walk on the GPU; it is here so a test can
/// pin the addressing without a device, which is exactly where an off-by-one
/// in the ring topology would otherwise hide until it showed as a missing
/// square of sea.
struct WaterPatchAddress {
    std::uint32_t level{0};
    /// Patch coordinate within the level, 0..WATER_CLIPMAP_RING-1 on both
    /// axes. Ring levels never produce a coordinate inside the hole.
    glm::uvec2 patch{0, 0};
};

/// Resolve a flat patch index to its level and patch coordinate.
///
/// Indices run level by level: level 0's full grid first, then each ring
/// level's patches in row-major order with the hole skipped. `hole_offset`
/// is where that level's hole starts, which moves with the camera.
[[nodiscard]] WaterPatchAddress water_patch_address(std::uint32_t patch_index,
                                                    std::uint32_t hole_offset);

} // namespace fjell
