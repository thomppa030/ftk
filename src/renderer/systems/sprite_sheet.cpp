#include "renderer/systems/sprite_sheet.hpp"
#include "ftk/base/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

SpriteSheet load_sprite_sheet(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open sprite sheet: " + path);
    }

    auto j = nlohmann::json::parse(f);
    SpriteSheet sheet;
    sheet.texture_path = j.value("texture", "");
    if (j.contains("frame_size")) {
        auto& fs = j["frame_size"];
        sheet.frame_size = {fs[0].get<int>(), fs[1].get<int>()};
    }
    sheet.frame_count = j.value("frame_count", 0u);
    sheet.columns = j.value("columns", 0u);
    sheet.fps = j.value("fps", 10.0f);
    sheet.loop = j.value("loop", true);

    return sheet;
}

void save_sprite_sheet(const std::string& path, const SpriteSheet& sheet) {
    nlohmann::json j;
    j["texture"] = sheet.texture_path;
    j["frame_size"] = {sheet.frame_size.x, sheet.frame_size.y};
    j["frame_count"] = sheet.frame_count;
    if (sheet.columns > 0) j["columns"] = sheet.columns;
    j["fps"] = sheet.fps;
    j["loop"] = sheet.loop;

    std::ofstream f(path);
    f << j.dump(4);
}

void compute_frame_uvs(SpriteSheet& sheet, uint32_t tex_width, uint32_t tex_height) {
    if (sheet.frame_size.x <= 0 || sheet.frame_size.y <= 0 ||
        tex_width == 0 || tex_height == 0) {
        return;
    }

    uint32_t cols = sheet.columns > 0
        ? sheet.columns
        : tex_width / static_cast<uint32_t>(sheet.frame_size.x);

    if (cols == 0) return;

    float fw = static_cast<float>(sheet.frame_size.x) / static_cast<float>(tex_width);
    float fh = static_cast<float>(sheet.frame_size.y) / static_cast<float>(tex_height);

    sheet.frame_uvs.clear();
    sheet.frame_uvs.reserve(sheet.frame_count);

    for (uint32_t i = 0; i < sheet.frame_count; ++i) {
        uint32_t col = i % cols;
        uint32_t row = i / cols;
        float u0 = static_cast<float>(col) * fw;
        float v0 = static_cast<float>(row) * fh;
        sheet.frame_uvs.push_back({u0, v0, u0 + fw, v0 + fh});
    }
}

} // namespace fjell
