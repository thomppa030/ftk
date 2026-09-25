#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

struct UiNode;
class UiContext;

/// Typed property declaration from <prop> tags in .fjwidget files.
struct PropDecl {
    std::string name;
    std::string type;          // "float", "int", "bool", "string"
    std::string default_value; // empty = required (no default)
};

/// Which node a Model's component is read from.
enum class ModelSource : uint8_t {
    self,  ///< the node the widget is placed on
    local, ///< the node carrying the component that the local player controls
};

/// A <model> in a .fjwidget: a game component the widget reads in place,
/// `{name.field}` in its markup. Only script components can be Models.
struct ModelDecl {
    std::string name;      ///< what markup calls it
    std::string component; ///< the script component's type name
    ModelSource from{ModelSource::self};
    std::string source_file; ///< where it is declared, for reports
    uint32_t source_line{0};
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
    std::vector<ModelDecl> models;          // components the widget reads
    std::vector<ActionDecl> actions;        // populated at runtime from C++ type info
    std::unique_ptr<UiNode> markup_root;    // parsed markup body (children of <widget>)
};

[[nodiscard]] Result<WidgetAsset> load_widget_asset(const std::string& path);

/// The <model>s a widget file declares, for whoever places the widget and
/// never needs its markup; empty when the file does not load.
[[nodiscard]] std::vector<ModelDecl> load_widget_models(const std::string& path);

/// Every declared prop that has a default, into the context as its declared
/// type: what a widget shows before anything binds its props.
void set_prop_defaults(const std::vector<PropDecl>& props, UiContext& context);

/// Writes the widget header, its <prop> and <model> declarations and its markup. The
/// markup is passed separately because the tree being saved is usually the
/// live one, with widgets resolved and editor-assigned ids, which the
/// serializer collapses back to authored form.
[[nodiscard]] Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset,
                                         const UiNode* markup);

/// The text save_widget_asset() writes, without writing it.
[[nodiscard]] std::string widget_asset_text(const WidgetAsset& asset, const UiNode* markup);

} // namespace fjell
