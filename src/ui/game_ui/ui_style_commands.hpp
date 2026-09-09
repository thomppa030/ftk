#pragma once

#include "core/command.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <string>
#include <utility>

namespace fjell {

/// Sets a single CSS-style property (e.g. "background-color") on the
/// #id-selector rule for a node, creating both the id (if the node has
/// none yet) and the rule (if none yet targets that id) on first use.
///
/// Style edits target #id rules rather than UiComputedStyle directly —
/// UiComputedStyle is a resolved cascade output with no single owning
/// rule, so there's nothing there to write back to .fjss (see
/// FjssSerializer). Routing every edit through one selector kind keeps
/// "which rule did this edit land in" unambiguous; a node's other
/// classes/tag rules are untouched and still apply underneath it.
class SetStylePropertyCommand : public Command {
public:
    SetStylePropertyCommand(GameUi& game_ui, UiNode& node,
                            std::string property, std::string old_value,
                            std::string new_value, std::string description)
        : game_ui_{game_ui}, node_{node}
        , property_{std::move(property)}
        , old_value_{std::move(old_value)}
        , new_value_{std::move(new_value)}
        , description_{std::move(description)} {
        // Auto-assign an id the first time a node is styled from the
        // inspector — every subsequent edit to this node reuses it. Left
        // in place even across undo (see undo() below).
        if (node_.id.empty()) {
            node_.id = make_unique_id();
        }
    }

    void execute() override {
        apply(new_value_);
    }

    void undo() override {
        apply(old_value_);
        // node_.id is deliberately left in place even if this command
        // assigned it — an editor-generated id is harmless to keep
        // around after undo, and removing it risks orphaning a rule a
        // later redo (or an unrelated command in between) still
        // expects to find by that id.
    }

    [[nodiscard]] std::string description() const override { return description_; }

private:
    void apply(const std::string& value) {
        auto& sheet = game_ui_.stylesheet();
        StyleRule* rule = find_rule(sheet);
        if (!rule) {
            StyleRule new_rule;
            new_rule.selector.id = node_.id;
            sheet.rules.push_back(std::move(new_rule));
            rule = &sheet.rules.back();
        }
        if (value.empty()) {
            rule->properties.erase(property_);
        } else {
            rule->properties[property_] = value;
        }
        game_ui_.mark_dirty();
    }

    [[nodiscard]] StyleRule* find_rule(Stylesheet& sheet) const {
        for (auto& rule : sheet.rules) {
            if (rule.selector.id == node_.id && rule.selector.tag.empty() &&
                rule.selector.class_.empty() && rule.selector.pseudo.empty() &&
                rule.selector.parent.empty()) {
                return &rule;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static std::string make_unique_id() {
        static uint32_t counter = 0;
        return "editor-node-" + std::to_string(++counter);
    }

    GameUi& game_ui_;
    UiNode& node_;
    std::string property_;
    std::string old_value_;
    std::string new_value_;
    std::string description_;
    std::string assigned_id_; // non-empty if this command assigned node_.id
};

} // namespace fjell
