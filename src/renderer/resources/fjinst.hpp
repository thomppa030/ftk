#pragma once

// .fjinst binary format — Fjell engine internal instance-set format.
// No engine dependencies — standard library only.
//
// The placements of one InstancedMeshComponent: where each copy of the model
// stands, in the owning node's local space. Bulk data, so it lives beside
// the scene as a binary file the scene references by path rather than as
// rows in the scene's JSON: a painted forest is one blob, not a hundred
// thousand changed lines.

#include <cstdint>
#include <vector>

namespace fjell {

static constexpr uint32_t FJINST_MAGIC   = 0x4E49464A; // "JFIN" little-endian
static constexpr uint32_t FJINST_VERSION = 1;

struct FjInstHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved[5];
};

// One placement. Rotation is a unit quaternion (x, y, z, w) and scale is per
// axis, kept apart rather than baked into a matrix so the editor can show and
// change them exactly.
//
// `anchor_node` is the scene node the placement was put on (0 for none) and
// `surface_offset` how far along that surface's normal it sits, so the
// placement can follow the surface when a terrain is sculpted under it.
struct FjInstRecord {
    float    position[3];
    float    rotation[4];
    float    scale[3];
    uint32_t anchor_node;
    float    surface_offset;
};
static_assert(sizeof(FjInstRecord) == 48);

struct FjInstData {
    FjInstHeader header{};
    std::vector<FjInstRecord> records;
};

} // namespace fjell
