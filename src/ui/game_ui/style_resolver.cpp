#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_style_properties.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <optional>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace fjell {

void StyleResolver::resolve(UiNode& root, const Stylesheet& sheet) {
    FJELL_PROFILE_SCOPE_N("style_resolve");
    active_sheet_ = &sheet;
    resolve_node(root, sheet, nullptr);
}

void StyleResolver::resolve_subtree(UiNode& node, const Stylesheet& sheet) {
    resolve_node(node, sheet, node.parent ? &node.parent->style : nullptr);
}

void StyleResolver::resolve_node(UiNode& node, const Stylesheet& sheet,
                                  const UiComputedStyle* parent_style) {
    apply_cascade(node, sheet, parent_style);
    for (auto& child : node.children) {
        resolve_node(*child, sheet, &node.style);
    }
}

std::unordered_map<std::string, StyleOrigin> StyleResolver::explain(const UiNode& node,
                                                                    const Stylesheet& sheet,
                                                                    float viewport_width,
                                                                    float viewport_height) {
    // The same ordering apply_cascade uses, so the last writer here is the
    // one whose value the node actually shows.
    struct Match {
        uint32_t specificity;
        const StyleRule* rule;
    };
    std::vector<Match> matches;
    for (const auto& rule : sheet.rules) {
        if (media_holds(rule.media, viewport_width, viewport_height) &&
            selector_matches(node, rule.selector)) {
            matches.push_back({rule.selector.specificity(), &rule});
        }
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.specificity < b.specificity; });

    std::unordered_map<std::string, StyleOrigin> origins;
    for (const auto& match : matches) {
        for (const auto& decl : match.rule->declarations) {
            origins[decl.name] = {match.rule, &decl, false};
        }
    }
    for (const auto& [property, value] : node.inline_styles) {
        origins[property] = {nullptr, nullptr, true};
    }
    return origins;
}

void StyleResolver::apply_cascade(UiNode& node, const Stylesheet& sheet,
                                  const UiComputedStyle* parent_style) {
    // Reset to defaults, then inherit from parent (ensures removed classes
    // don't leave stale properties like gradients or box shadows).
    node.style = UiComputedStyle{};
    if (parent_style) {
        inherit_properties(node.style, *parent_style);
    }
    parent_font_size_ = parent_style ? parent_style->font_size : UiComputedStyle{}.font_size;
    if (!parent_style) root_font_size_ = UiComputedStyle{}.font_size;

    struct Match {
        uint32_t specificity;
        const StyleRule* rule;
    };
    std::vector<Match> matches;
    for (const auto& rule : sheet.rules) {
        // A rule under @media only takes part while its conditions hold for
        // the frame; a resize re-runs the cascade.
        if (!media_holds(rule.media, viewport_width_, viewport_height_)) continue;
        if (selector_matches(node, rule.selector)) {
            matches.push_back({rule.selector.specificity(), &rule});
        }
    }

    // Lower specificity first so higher specificity overwrites; the stable
    // sort keeps sheet order for equal specificity, and within one rule the
    // declarations apply in the order they were written.
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.specificity < b.specificity; });

    // Custom properties first, from every matching rule and the node's own
    // style-* attributes, so a var() anywhere below sees the final values
    // whatever order the rules came in.
    for (const auto& match : matches) {
        for (const auto& decl : match.rule->declarations) {
            if (decl.name.starts_with("--")) node.style.set_custom_property(decl.name, decl.value);
        }
    }
    for (const auto& [property, value] : node.inline_styles) {
        if (property.starts_with("--")) node.style.set_custom_property(property, value);
    }

    auto apply = [&](const std::string& name, const std::string& value) {
        if (name.starts_with("--")) return;
        if (value.find("var(") == std::string::npos) {
            apply_property(node.style, name, value);
            return;
        }
        // A var() that cannot be resolved leaves the declaration out, the
        // way CSS treats a value invalid at computed-value time.
        if (auto substituted = substitute_variables(node.style, value)) {
            apply_property(node.style, name, *substituted);
        }
    };
    for (const auto& match : matches) {
        for (const auto& decl : match.rule->declarations) {
            apply(decl.name, decl.value);
        }
    }

    // Inline styles, from style-* attributes with their templates resolved
    for (const auto& [property, value] : node.inline_styles) {
        apply(property, value);
    }

    // The root's computed size is what rem means for everything below it.
    if (!parent_style) root_font_size_ = node.style.font_size;
}

float StyleResolver::resolve_font_size(UiLength authored) const {
    if (authored.is_auto()) return parent_font_size_;
    if (authored.is_percent()) return parent_font_size_ * authored.value / 100.0f;
    UiLengthContext ctx{parent_font_size_, root_font_size_, viewport_width_, viewport_height_};
    return resolve_relative(authored, ctx).value;
}

void StyleResolver::inherit_properties(UiComputedStyle& style, const UiComputedStyle& parent) {
    for (const auto& desc : ui_style_properties()) {
        if (desc.inherited) ui_style_copy(style, parent, desc);
    }
    // Every custom property inherits, as in CSS
    style.custom_properties = parent.custom_properties;
}

bool StyleResolver::compound_matches(const UiNode& node, const StyleCompound& c) {
    if (!c.id.empty() && node.id != c.id) return false;
    if (!c.tag.empty() && node.tag != c.tag) return false;
    for (const auto& cls : c.classes) {
        if (!node.has_class(cls)) return false;
    }
    for (const auto& pseudo : c.pseudo_classes) {
        if (pseudo == "hover" && !node.is_hovered()) return false;
        if (pseudo == "active" && !node.is_active()) return false;
        if (pseudo == "focus" && !node.is_focused()) return false;
        if (pseudo == "root" && node.parent != nullptr) return false;
    }
    return true;
}

std::optional<std::string> StyleResolver::substitute_variables(const UiComputedStyle& style,
                                                               std::string value) {
    // Each var(--name[, fallback]) is replaced in place; a fallback may hold
    // another var(), which the next round picks up.
    for (int round = 0; round < 8; ++round) {
        auto start = value.find("var(");
        if (start == std::string::npos) return value;
        int depth = 0;
        size_t end = start + 3;
        for (; end < value.size(); ++end) {
            if (value[end] == '(') ++depth;
            else if (value[end] == ')' && --depth == 0) break;
        }
        if (end >= value.size()) return std::nullopt; // unbalanced
        std::string inner = value.substr(start + 4, end - start - 4);
        auto comma = inner.find(',');
        std::string name = inner.substr(0, comma);
        auto trim = [](std::string s) {
            auto b = s.find_first_not_of(" \t");
            auto e = s.find_last_not_of(" \t");
            return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
        };
        name = trim(name);
        std::string replacement;
        if (const auto* found = style.custom_property(name)) {
            replacement = *found;
        } else if (comma != std::string::npos) {
            replacement = trim(inner.substr(comma + 1));
        } else {
            return std::nullopt; // no such variable and nothing to fall back to
        }
        value.replace(start, end - start + 1, replacement);
    }
    return std::nullopt;
}

bool StyleResolver::ancestors_match(const UiNode* current,
                                    const std::vector<StyleAncestorStep>& steps,
                                    size_t remaining) {
    if (remaining == 0) return true;
    const auto& step = steps[remaining - 1];
    if (step.combinator == UiCombinator::child) {
        return current != nullptr && compound_matches(*current, step.compound) &&
               ancestors_match(current->parent, steps, remaining - 1);
    }
    // Descendant: try every ancestor that fits and let the steps further
    // out decide, so `a > b c` still matches when the nearest `b` above `c`
    // is not a child of an `a` but a farther one is.
    for (; current != nullptr; current = current->parent) {
        if (compound_matches(*current, step.compound) &&
            ancestors_match(current->parent, steps, remaining - 1)) {
            return true;
        }
    }
    return false;
}

bool StyleResolver::selector_matches(const UiNode& node, const StyleSelector& sel) {
    if (!compound_matches(node, sel.subject)) return false;
    return ancestors_match(node.parent, sel.ancestors, sel.ancestors.size());
}

void StyleResolver::apply_property(UiComputedStyle& style, const std::string& name,
                                   const std::string& value) {
    const auto* desc = find_ui_style_property(name);
    // Unknown names were reported when the sheet was parsed.
    if (!desc) return;

    // font-size is authored as a length but stored resolved: children
    // inherit the pixel value and em on the node itself builds on it.
    if (desc->name == "font-size") {
        style.font_size = resolve_font_size(parse_length(value));
        return;
    }
    // font-weight takes keywords as well as numbers; a bad value was
    // reported at parse time and leaves the inherited weight alone.
    if (desc->name == "font-weight") {
        if (auto weight = parse_ui_font_weight(value)) style.font_weight = *weight;
        return;
    }

    switch (desc->kind) {
    case UiPropertyKind::number:
        style.*std::get<float UiComputedStyle::*>(desc->member) = parse_px(value);
        return;
    case UiPropertyKind::string:
        style.*std::get<std::string UiComputedStyle::*>(desc->member) = ui_style_unquote(value);
        return;
    case UiPropertyKind::length: {
        auto length = parse_length(value);
        if (length.is_auto() && !desc->auto_allowed) return; // reported at parse time
        style.*std::get<UiLength UiComputedStyle::*>(desc->member) = length;
        return;
    }
    case UiPropertyKind::color:
        style.*std::get<UiColor UiComputedStyle::*>(desc->member) = parse_color(value);
        return;
    case UiPropertyKind::edges: {
        auto edges = parse_edges(value);
        bool any_auto = edges.top.is_auto() || edges.right.is_auto() ||
                        edges.bottom.is_auto() || edges.left.is_auto();
        if (any_auto && !desc->auto_allowed) return; // reported at parse time
        style.*std::get<UiEdges UiComputedStyle::*>(desc->member) = edges;
        return;
    }
    case UiPropertyKind::edge: {
        auto length = parse_length(value);
        if (length.is_auto() && !desc->auto_allowed) return;
        ui_edge(style.*std::get<UiEdges UiComputedStyle::*>(desc->member), desc->side) = length;
        return;
    }
    case UiPropertyKind::corners:
        style.*std::get<UiCorners UiComputedStyle::*>(desc->member) = parse_corners(value);
        return;
    case UiPropertyKind::keyword:
        for (size_t i = 0; i < desc->keywords.size(); ++i) {
            if (desc->keywords[i] == value) {
                desc->set_keyword(style, static_cast<uint8_t>(i));
                return;
            }
        }
        return; // a misspelt keyword was reported at parse time
    case UiPropertyKind::composite:
        apply_composite(style, *desc, value);
        return;
    }
}

void StyleResolver::apply_composite(UiComputedStyle& style, const UiPropertyDescriptor& desc,
                                    const std::string& value) {
    if (desc.name == "gap") {
        auto length = parse_length(value);
        if (length.is_auto()) return;
        style.row_gap = style.column_gap = length;
        return;
    }

    // Background: gradient or solid color
    if (desc.name == "background") {
        if (value.find("linear-gradient(") == 0) {
            auto inner = value.substr(16, value.size() - 17);
            std::vector<std::string> parts;
            std::string part;
            int paren_depth = 0;
            for (char c : inner) {
                if (c == '(') paren_depth++;
                if (c == ')') paren_depth--;
                if (c == ',' && paren_depth == 0) {
                    parts.push_back(part);
                    part.clear();
                } else {
                    part += c;
                }
            }
            if (!part.empty()) parts.push_back(part);

            for (auto& p : parts) {
                auto s = p.find_first_not_of(" \t");
                auto e = p.find_last_not_of(" \t");
                if (s != std::string::npos) p = p.substr(s, e - s + 1);
            }

            if (parts.size() >= 3) {
                float angle = 180.0f;
                if (parts[0].find("deg") != std::string::npos) {
                    angle = parse_px(parts[0]);
                } else if (parts[0] == "to bottom") { angle = 180.0f; }
                else if (parts[0] == "to top") { angle = 0.0f; }
                else if (parts[0] == "to right") { angle = 90.0f; }
                else if (parts[0] == "to left") { angle = 270.0f; }

                style.gradient.angle = angle;
                style.gradient.from = parse_color(parts[1]);
                style.gradient.to = parse_color(parts[2]);
            } else if (parts.size() == 2) {
                style.gradient.angle = 180.0f;
                style.gradient.from = parse_color(parts[0]);
                style.gradient.to = parse_color(parts[1]);
            }
            return;
        }
        style.background_color = parse_color(value);
        return;
    }

    // Border shorthand: "2px solid #333" or "1 #666"
    if (desc.name == "border") {
        std::istringstream iss(value);
        std::string token;
        while (iss >> token) {
            if (token == "solid" || token == "none" || token == "dashed") continue;
            if (token[0] == '#' || token.find("rgb") == 0 ||
                token == "transparent" || token == "white" || token == "black" ||
                token == "red" || token == "green" || token == "blue") {
                style.border_color = parse_color(token);
            } else {
                style.border_width = parse_px(token);
            }
        }
        return;
    }

    // Transition: stored for UiAnimation to process
    if (desc.name == "transition") {
        style.transition = value;
        return;
    }

    // Box shadow: "offset_x offset_y blur spread #color"
    // e.g. "0 4 8 0 rgba(0, 0, 0, 0.3)" or "2 2 6 #00000080"
    if (desc.name == "box-shadow") {
        if (value == "none") { style.box_shadow = {}; return; }
        std::istringstream iss(value);
        std::string token;
        std::vector<std::string> tokens;
        while (iss >> token) tokens.push_back(token);

        // Rejoin rgba(...) tokens that got split
        std::vector<std::string> merged;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i].find("rgba(") == 0 || tokens[i].find("rgb(") == 0) {
                std::string joined = tokens[i];
                while (i + 1 < tokens.size() && joined.back() != ')') {
                    ++i;
                    joined += " " + tokens[i];
                }
                merged.push_back(joined);
            } else {
                merged.push_back(tokens[i]);
            }
        }

        if (merged.size() >= 4) {
            style.box_shadow.offset_x = parse_px(merged[0]);
            style.box_shadow.offset_y = parse_px(merged[1]);
            style.box_shadow.blur = parse_px(merged[2]);
            if (merged.size() >= 5 && (merged[3][0] != '#' && merged[3].find("rgb") != 0)) {
                style.box_shadow.spread = parse_px(merged[3]);
                style.box_shadow.color = parse_color(merged[4]);
            } else {
                style.box_shadow.color = parse_color(merged[3]);
            }
        }
        return;
    }
}

UiColor StyleResolver::parse_color(const std::string& value) {
    if (value.empty()) return {};

    // Hex color: #RGB, #RRGGBB, #RRGGBBAA
    if (value[0] == '#') {
        std::string hex = value.substr(1);
        if (hex.size() == 3) {
            std::string expanded;
            for (char c : hex) { expanded += c; expanded += c; }
            hex = expanded;
        }

        uint32_t val = 0;
        std::from_chars(hex.data(), hex.data() + hex.size(), val, 16);

        if (hex.size() == 8) {
            return UiColor::from_hex_alpha(val);
        }
        return UiColor::from_hex(val);
    }

    // rgba(r, g, b, a) — values 0-255 for rgb, 0.0-1.0 for alpha
    if (value.find("rgba(") == 0) {
        auto inner = value.substr(5, value.size() - 6); // strip "rgba(" and ")"
        std::istringstream iss(inner);
        float r = 0, g = 0, b = 0, a = 1;
        char comma;
        iss >> r >> comma >> g >> comma >> b >> comma >> a;
        return {r / 255.0f, g / 255.0f, b / 255.0f, a};
    }

    // rgb(r, g, b)
    if (value.find("rgb(") == 0) {
        auto inner = value.substr(4, value.size() - 5);
        std::istringstream iss(inner);
        float r = 0, g = 0, b = 0;
        char comma;
        iss >> r >> comma >> g >> comma >> b;
        return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    }

    // Named colors
    if (value == "transparent") return {0, 0, 0, 0};
    if (value == "white") return {1, 1, 1, 1};
    if (value == "black") return {0, 0, 0, 1};
    if (value == "red") return {1, 0, 0, 1};
    if (value == "green") return {0, 0.5f, 0, 1};
    if (value == "blue") return {0, 0, 1, 1};
    if (value == "yellow") return {1, 1, 0, 1};
    if (value == "cyan") return {0, 1, 1, 1};
    if (value == "magenta") return {1, 0, 1, 1};
    if (value == "gray" || value == "grey") return {0.5f, 0.5f, 0.5f, 1};
    if (value == "orange") return {1, 0.647f, 0, 1};

    return {};
}

float StyleResolver::parse_px(const std::string& value) {
    float result = 0.0f;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}

UiLength StyleResolver::parse_length(const std::string& value) {
    if (value == "auto") return UiLength::automatic();
    float number = 0.0f;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
    // Whatever follows the number is the unit, matched whole: "rem" must
    // not be mistaken for a suffix of "em".
    std::string_view unit{end, static_cast<size_t>(value.data() + value.size() - end)};
    for (UiUnit candidate : {UiUnit::percent, UiUnit::em, UiUnit::rem, UiUnit::vw, UiUnit::vh,
                             UiUnit::vmin, UiUnit::vmax}) {
        if (unit == unit_suffix(candidate)) return {number, candidate};
    }
    return UiLength::pixels(number);
}

UiEdges StyleResolver::parse_edges(const std::string& value) {
    std::vector<UiLength> values;
    std::istringstream iss(value);
    std::string token;
    while (iss >> token) {
        values.push_back(parse_length(token));
    }

    UiEdges edges{};
    if (values.size() == 1) {
        edges.top = edges.right = edges.bottom = edges.left = values[0];
    } else if (values.size() == 2) {
        edges.top = edges.bottom = values[0];
        edges.right = edges.left = values[1];
    } else if (values.size() == 3) {
        edges.top = values[0];
        edges.right = edges.left = values[1];
        edges.bottom = values[2];
    } else if (values.size() >= 4) {
        edges.top = values[0];
        edges.right = values[1];
        edges.bottom = values[2];
        edges.left = values[3];
    }
    return edges;
}

UiCorners StyleResolver::parse_corners(const std::string& value) {
    std::vector<float> values;
    std::istringstream iss(value);
    std::string token;
    while (iss >> token) {
        values.push_back(parse_px(token));
    }

    UiCorners corners{};
    if (values.size() == 1) {
        corners.top_left = corners.top_right = corners.bottom_right = corners.bottom_left = values[0];
    } else if (values.size() == 2) {
        corners.top_left = corners.bottom_right = values[0];
        corners.top_right = corners.bottom_left = values[1];
    } else if (values.size() >= 4) {
        corners.top_left = values[0];
        corners.top_right = values[1];
        corners.bottom_right = values[2];
        corners.bottom_left = values[3];
    }
    return corners;
}

} // namespace fjell
