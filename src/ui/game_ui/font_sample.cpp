#include "ui/game_ui/font_sample.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/text_shaper.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <cmath>
#include <filesystem>
#include <memory>

namespace fjell {

void load_font_sample(GameUi& ui, const std::string& recipe_path, const std::string& text,
                      float font_size, const FontAtlas* atlas) {
    // The box the line needs, from the same shaping the renderer will do.
    float width = font_size * 0.6f * static_cast<float>(text.size());
    float height = font_size * 1.3f;
    if (atlas && atlas->valid()) {
        auto shaped = TextShaper::shape(text, *atlas, font_size);
        width = shaped.width;
        height = shaped.height;
    }
    width = std::ceil(width) + 2.0f;
    height = std::ceil(height) + 2.0f;

    auto root = std::make_unique<UiNode>();
    root->tag = "div";
    auto box = std::make_unique<UiNode>();
    box->tag = "div";
    box->id = "sample";
    box->parent = root.get();
    auto line = std::make_unique<UiNode>();
    line->type = UiNodeType::text;
    line->text_content = text;
    line->parent = box.get();
    box->children.push_back(std::move(line));
    root->children.push_back(std::move(box));

    auto sheet = FjssParser::parse(
        "@font-face { font-family: Sample; src: \"sample\"; }\n"
        "#sample { font-family: Sample; font-size: " + std::to_string(font_size) +
        "; width: " + std::to_string(width) + "; height: " + std::to_string(height) +
        "; color: white; }\n",
        recipe_path);
    // The face is bound straight to the recipe; the loader never sees it.
    for (auto& face : sheet.font_faces) face.resolved_src = recipe_path;
    ui.load_from_tree(std::move(root), std::move(sheet),
                      std::filesystem::path(recipe_path).parent_path().string());
}

} // namespace fjell
