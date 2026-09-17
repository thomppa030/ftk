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
/// Six, which is exactly the inner level's own span: a level is RING patches
/// of its own size, and the level outside it is twice that size, so the inner
/// level measures RING/2 of the outer one's patches. Because every level
/// snaps in pairs of its patches, the two always line up on a whole patch —
/// the hole and the level filling it have the same edges at every camera
/// position, with no gap and no overlap.
///
/// Where the hole sits still moves with the camera; only its size is fixed.
/// See `WaterClipmap::recenter()`.
inline constexpr std::uint32_t WATER_CLIPMAP_HOLE = WATER_CLIPMAP_RING / 2;

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
    /// Which patch this level's hole starts at, per axis, packed low half x
    /// and high half y.
    ///
    /// The two axes differ: each snaps on its own, so the inner level sits
    /// three of this level's patches in along one and four along the other
    /// most of the time. Packed rather than a second vec2 because the level
    /// is a vec4 and a fifth float would double its std140 stride.
    ///
    /// Zero on level 0, which has no hole. Read through `hole_offset_xz()`;
    /// `fjell_water_hole_offset()` is the shader's half of the same unpack.
    std::uint32_t hole_offset{0};

    /// The packed offset as a pair. The shader unpacks it the same way.
    [[nodiscard]] glm::uvec2 hole_offset_xz() const {
        return {hole_offset & 0xFFFFU, hole_offset >> 16U};
    }

    /// Pack a per-axis offset into `hole_offset`.
    void set_hole_offset(glm::uvec2 offset) {
        hole_offset = (offset.x & 0xFFFFU) | (offset.y << 16U);
    }
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
/// **Levels snap in pairs of patches, not in cells.** A level whose origin
/// moved by one cell would shift its patch boundaries by a fraction of a
/// patch, so a ring would stop being a ring; snapping in whole patches fixes
/// that and is what stops the sea crawling as the camera moves, since a
/// vertex holds its world position and so holds its place in the wave field.
///
/// Pairs rather than single patches because a patch is 11 cells wide, an odd
/// number: a level at an odd multiple of its own cell size has its edge
/// falling between two of the coarser level's vertices, which is a crack with
/// nothing to close it against. Stepping in pairs keeps every level's origin
/// at an even cell, which is what the mesh shader's seam stitch and the exact
/// hole fit both rest on.
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
/// is where that level's hole starts on each axis, which moves with the
/// camera and is not the same on both.
[[nodiscard]] WaterPatchAddress water_patch_address(std::uint32_t patch_index,
                                                    glm::uvec2 hole_offset);

} // namespace fjell
