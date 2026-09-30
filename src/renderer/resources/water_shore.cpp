#include "renderer/resources/water_shore.hpp"

#include "renderer/resources/fjterrain.hpp"

#include <algorithm>

namespace fjell::water {

std::optional<StillDepth> still_depth(const ShoreBed& bed, glm::vec2 world_xz) {
    if (bed.terrain == nullptr) return std::nullopt;
    const FjTerrainData& terrain = *bed.terrain;
    const std::uint32_t samples = terrain.sample_count();
    const float size = terrain.header.world_size;
    if (samples < 2 || size <= 0.0f
        || terrain.heights.size() < static_cast<std::size_t>(samples) * samples) {
        return std::nullopt;
    }

    // Field coordinates, metres from the min corner. Off the field there is
    // no bed to speak of, rather than the edge's height carried on outward.
    const glm::vec2 field = world_xz - bed.min_corner;
    if (field.x < 0.0f || field.y < 0.0f || field.x > size || field.y > size) {
        return std::nullopt;
    }

    const auto quads = static_cast<float>(samples - 1);
    const float per_metre = quads / size;
    const float fx = std::min(field.x * per_metre, quads);
    const float fz = std::min(field.y * per_metre, quads);
    // The last row and column start no cell of their own, so a point on the
    // far edge reads the cell before it, at its far side.
    const auto x0 = std::min(static_cast<std::uint32_t>(fx), samples - 2);
    const auto z0 = std::min(static_cast<std::uint32_t>(fz), samples - 2);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const auto at = [&](std::uint32_t x, std::uint32_t z) {
        return terrain.heights[(static_cast<std::size_t>(z) * samples) + x];
    };
    const float h00 = at(x0, z0);
    const float h10 = at(x0 + 1, z0);
    const float h01 = at(x0, z0 + 1);
    const float h11 = at(x0 + 1, z0 + 1);

    const float top = h00 + ((h10 - h00) * tx);
    const float bottom = h01 + ((h11 - h01) * tx);
    const float normalized = top + ((bottom - top) * tz);

    // The bed rises as the normalized height does; the depth falls with it.
    const float rise = terrain.header.height_scale * per_metre;
    const float slope_x = ((h10 - h00) * (1.0f - tz)) + ((h11 - h01) * tz);
    const float slope_z = bottom - top;

    StillDepth out;
    out.depth = bed.surface_y - (bed.ground_y + terrain.world_height(normalized));
    out.gradient = glm::vec2(-slope_x, -slope_z) * rise;
    return out;
}

} // namespace fjell::water
