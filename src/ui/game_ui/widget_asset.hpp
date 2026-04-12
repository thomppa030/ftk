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
[[nodiscard]] Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset);

} // namespace fjell
