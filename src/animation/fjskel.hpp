#pragma once

// .fjskel binary format — Fjell engine skeleton format.
// Shared between the engine runtime and the fjimport tool.
// The skeleton blob is an ozz::animation::Skeleton serialized via ozz archives.
// Inverse bind matrices are stored separately (not part of ozz data).

#include <cstdint>

namespace fjell {

static constexpr uint32_t FJSKEL_MAGIC   = 0x4B534A46; // "FJSK" little-endian
static constexpr uint32_t FJSKEL_VERSION = 1;

struct FjSkelHeader {
    uint32_t magic;              // FJSKEL_MAGIC
    uint32_t version;            // FJSKEL_VERSION
    uint32_t num_joints;
    uint32_t pad;

    // Section byte offsets from start of file
    uint64_t skeleton_offset;    // -> ozz serialized skeleton blob
    uint64_t skeleton_size;      // size in bytes
    uint64_t inv_bind_offset;    // -> float[16 * num_joints] (column-major mat4)
};

} // namespace fjell
