#include "ui/game_ui/font_sample.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <filesystem>
#include <memory>

namespace fjell {

void load_font_sample(GameUi& ui, const std::string& recipe_path, const std::string& text,
                      float font_size) {
    auto root = std::make_unique<UiNode>();
    root->tag = "div";
    root->id = "sample";
    auto line = std::make_unique<UiNode>();
    line->type = UiNodeType::text;
    line->text_content = text;
    line->parent = root.get();
    root->children.push_back(std::move(line));

    auto sheet = FjssParser::parse(
        "@font-face { font-family: Sample; src: \"sample\"; }\n"
        "#sample { font-family: Sample; font-size: " + std::to_string(font_size) +
        "; color: white; align-items: flex-start; justify-content: flex-start; }\n",
        recipe_path);
    // The face is bound straight to the recipe; the loader never sees it.
    for (auto& face : sheet.font_faces) face.resolved_src = recipe_path;
    ui.load_from_tree(std::move(root), std::move(sheet),
                      std::filesystem::path(recipe_path).parent_path().string());
}

} // namespace fjell
