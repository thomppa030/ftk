#pragma once

#include "core/result.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fjell {

struct UiNode;

/// Typed property declaration from <prop> tags in .fjwidget files.
struct PropDecl {
    std::string name;
    std::string type;          // "float", "int", "bool", "string"
    std::string default_value; // empty = required (no default)
};

/// Action declaration — populated from C++ [[fj::action]] metadata at runtime.
struct ActionDecl {
    std::string name;
    std::string param_type;    // "", "int", "float", "vec2"
};

struct WidgetAsset {
    std::string class_name;                 // optional C++ controller class
    std::string fjss_path;                  // relative path to .fjss stylesheet
    std::vector<PropDecl> props;            // typed prop declarations
    std::vector<ActionDecl> actions;        // populated at runtime from C++ type info
    std::unique_ptr<UiNode> markup_root;    // parsed markup body (children of <widget>)
};

[[nodiscard]] Result<WidgetAsset> load_widget_asset(const std::string& path);

/// Writes the widget header, its <prop> declarations and its markup. The
/// markup is passed separately because the tree being saved is usually the
/// live one, with widgets resolved and editor-assigned ids, which the
/// serializer collapses back to authored form.
[[nodiscard]] Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset,
                                         const UiNode* markup);

/// The text save_widget_asset() writes, without writing it.
[[nodiscard]] std::string widget_asset_text(const WidgetAsset& asset, const UiNode* markup);

} // namespace fjell
