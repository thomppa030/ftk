#pragma once

// .fjanim binary format — Fjell engine animation clip format.
// Shared between the engine runtime and the fjimport tool.
// The data blob is an ozz::animation::Animation serialized via ozz archives.

#include <cstdint>

namespace fjell {

static constexpr uint32_t FJANIM_MAGIC   = 0x4E414A46; // "FJAN" little-endian
static constexpr uint32_t FJANIM_VERSION = 1;

struct FjAnimHeader {
    uint32_t magic;              // FJANIM_MAGIC
    uint32_t version;            // FJANIM_VERSION
    uint32_t num_tracks;         // must match skeleton num_joints
    float duration;              // clip duration in seconds
    uint32_t name_length;        // length of clip name string (no null terminator)
    uint32_t pad;

    // Section byte offsets from start of file
    uint64_t name_offset;        // -> char[name_length]
    uint64_t data_offset;        // -> ozz compressed animation blob
    uint64_t data_size;          // size of the ozz blob in bytes
};

} // namespace fjell
