#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct TilemapComponent : NodeComponent {
    std::string tileset_texture;        // PNG path to tileset atlas
    glm::ivec2 tile_size{64, 64};       // pixel size of each tile in the tileset
    glm::ivec2 map_size{16, 16};        // map dimensions in tiles
    float world_tile_size{1.0f};        // world units per tile
    int32_t sort_layer{-100};           // render below sprites by default
    std::vector<int32_t> tiles;         // flat row-major, -1 = empty
    bool pixel_art{true};

    // Runtime (not serialized)
    uint32_t texture_index{0};
    uint32_t tileset_columns{0};        // computed from texture width / tile_size.x
    uint32_t tex_width{0};
    uint32_t tex_height{0};
    bool texture_loaded{false};
    bool dirty{true};                   // needs SSBO re-upload
};

} // namespace fjell
