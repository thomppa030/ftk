#pragma once

#include <array>
#include <string_view>

namespace fjell {

struct AssetTemplate {
    const char* label;       // Menu display name (nullptr = separator)
    std::string_view filename;  // Default name without extension
    std::string_view extension;
    std::string_view content;   // Template file content
};

inline constexpr std::array kAssetTemplates = {
    // ── Input ──
    AssetTemplate{"Input Action (.fjaction)", "new_action", ".fjaction",
                  R"({"name":"new_action","type":"button"})"},
    AssetTemplate{"Input Mapping (.fjinput)", "new_context", ".fjinput",
                  R"({"name":"new_context","consume":true,"bindings":[]})"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── Materials & Shaders ──
    AssetTemplate{"Material (.fjmat)", "new_material", ".fjmat",
                  R"({"shader":"shaders/standard.fjsl","textures":{},"params":{}})"},
    AssetTemplate{"Spatial Shader (.fjsl)", "new_shader", ".fjsl",
                  "shader_type spatial;\nvoid fragment() {\n}\n"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── Physics ──
    AssetTemplate{"Surface (.fjsurface)", "new_surface", ".fjsurface",
                  R"({"name":"new_surface"})"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── Environment ──
    AssetTemplate{"Day Profile (.fjday)", "new_day", ".fjday",
                  R"({"name":"new_day","tracks":{"length":24,"wrap":true,"tracks":[]}})"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── VFX ──
    AssetTemplate{"VFX Effect (.fjvfx)", "new_effect", ".fjvfx",
                  R"({"version":2,"name":"new_effect","emitters":[]})"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── UI ──
    AssetTemplate{"UI Markup (.fjui)", "new_ui", ".fjui",
                  "<panel id=\"root\">\n</panel>\n"},
    AssetTemplate{"UI Stylesheet (.fjss)", "new_style", ".fjss",
                  "#root {\n    flex-direction: column;\n    padding: 8;\n}\n"},
    AssetTemplate{"Widget (.fjwidget)", "new_widget", ".fjwidget",
                  "<widget style=\"new_widget.fjss\">\n"
                  "    <panel id=\"root\">\n"
                  "    </panel>\n"
                  "</widget>\n"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── Animation ──
    AssetTemplate{"Animation Set (.fjanimset)", "new_animset", ".fjanimset",
                  R"({"skeleton":"","initial":"","params":{},"states":[]})"},
    AssetTemplate{"Blend Space 1D (.fjblend1D)", "new_blend1d", ".fjblend1D",
                  R"({"skeleton":"","parameter":"speed","points":[]})"},
    AssetTemplate{"Blend Space 2D (.fjblend2D)", "new_blend2d", ".fjblend2D",
                  R"({"skeleton":"","parameter_x":"forward","parameter_y":"strafe","points":[]})"},
    AssetTemplate{nullptr, {}, {}, {}},
    // ── Scene ──
    AssetTemplate{"Scene (.fjell)", "new_scene", ".fjell",
                  R"({"objects":[]})"},
    AssetTemplate{"Preset (.fjp)", "new_preset", ".fjp",
                  R"({"preset":{"name":"new_preset"},"next_id":1,"objects":[]})"},
};

} // namespace fjell
