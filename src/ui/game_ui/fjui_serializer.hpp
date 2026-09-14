#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace fjell {

/// Writes a UiNode tree back to .fjui markup text — the inverse of
/// FjuiParser. Used by the visual UI editor to persist canvas/inspector
/// edits without disturbing the rest of a hand-authored file's structure
/// any more than necessary.
///
/// Only serializes authored state (tag, id, classes, attributes, text,
/// children) — never UiComputedStyle, which is a resolved cascade output
/// with no single owning rule to write back to (see FjssSerializer for
/// that side of persistence).
///
/// Widget references collapse back to their original <tag/> form instead
/// of expanding into WidgetResolver's resolved subtree: a node with
/// is_widget=true is written as <widget_tag prop="expr" on:event="action"/>
/// (or with children, if it captured slot content), never as the cloned
/// widget markup living under it. Resolver-injected scope classes
/// (__w_*) and foreach clone/template bookkeeping are never written —
/// they're runtime artifacts, not authored markup.
class FjuiSerializer {
public:
    /// Serialize a resolved UiNode tree back to .fjui source text.
    /// `root` may be the true root or (per FjuiParser's single-root
    /// promotion) a lone top-level element — both round-trip correctly.
    [[nodiscard]] static std::string serialize(const UiNode& root);

private:
    static void write_node(const UiNode& node, std::string& out, int indent);
    static void write_attributes(const UiNode& node, std::string& out);
    static std::string escape_attribute_value(const std::string& value);
};

/// Writes a Stylesheet back to .fjss text — the inverse of FjssParser.
/// Operates purely on StyleRule/StyleSelector — never on UiNode or
/// UiComputedStyle — since a resolved style has no single authored rule
/// to point back to once the cascade has run. Editor edits append or
/// modify StyleRule entries directly (see the editor's #id-selector
/// convention for canvas-driven edits) and this only has to turn that
/// rule list back into text.
class FjssSerializer {
public:
    /// Every rule in the sheet, whatever file it came from.
    [[nodiscard]] static std::string serialize(const Stylesheet& sheet);
    /// Only the rules that belong to `source` — what saving that file
    /// writes, leaving the widget sheets merged into the same Stylesheet
    /// to their own files.
    [[nodiscard]] static std::string serialize(const Stylesheet& sheet, std::string_view source);

private:
    static std::string write_selector(const StyleSelector& sel);
    static void write_rules(const Stylesheet& sheet, std::optional<std::string_view> source,
                            std::string& out);
    static void write_rule(const StyleRule& rule, std::string& out, int indent);
    /// The @import lines of `source`, or of every file when source is empty.
    static void write_imports(const Stylesheet& sheet, std::optional<std::string_view> source,
                              std::string& out);
    static std::string finish(std::string out);
};

} // namespace fjell
