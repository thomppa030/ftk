#include "ui/game_ui/style_resolver.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace fjell {

void StyleResolver::resolve(UiNode& root, const Stylesheet& sheet) {
    FJELL_PROFILE_SCOPE_N("style_resolve");
    active_sheet_ = &sheet;
    resolve_node(root, sheet, nullptr);
}

void StyleResolver::resolve_single(UiNode& node, const Stylesheet& sheet) {
    // Reset to defaults, then re-inherit from parent
    UiComputedStyle fresh{};
    if (node.parent) {
        inherit_properties(fresh, node.parent->style);
    }
    node.style = fresh;

    // Collect and apply matching rules
    struct Match {
        uint32_t specificity;
        const std::unordered_map<std::string, std::string>* props;
    };
    std::vector<Match> matches;

    for (const auto& rule : sheet.rules) {
        if (selector_matches(node, rule.selector)) {
            matches.push_back({rule.selector.specificity(), &rule.properties});
        }
    }

    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.specificity < b.specificity; });

    for (const auto& match : matches) {
        for (const auto& [name, value] : *match.props) {
            apply_property(node.style, name, value);
        }
    }

    // Inline style attributes
    for (const auto& [name, value] : node.attributes) {
        if (name == "id" || name == "class" || name == "style") continue;
        if (name.find("style-") == 0) {
            apply_property(node.style, name.substr(6), value);
        }
    }
}

void StyleResolver::resolve_node(UiNode& node, const Stylesheet& sheet,
                                  const UiComputedStyle* parent_style) {
    // Reset to defaults, then inherit from parent (ensures removed classes
    // don't leave stale properties like gradients or box shadows).
    node.style = UiComputedStyle{};
    if (parent_style) {
        inherit_properties(node.style, *parent_style);
    }

    // Collect matching rules with specificity
    struct Match {
        uint32_t specificity;
        const std::unordered_map<std::string, std::string>* props;
    };
    std::vector<Match> matches;

    for (const auto& rule : sheet.rules) {
        if (selector_matches(node, rule.selector)) {
            matches.push_back({rule.selector.specificity(), &rule.properties});
        }
    }

    // Sort by specificity (lower first, so higher specificity wins)
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.specificity < b.specificity; });

    // Apply properties in order (higher specificity overwrites)
    for (const auto& match : matches) {
        for (const auto& [name, value] : *match.props) {
            apply_property(node.style, name, value);
        }
    }

    // Inline style attributes (style-*)
    for (const auto& [name, value] : node.attributes) {
        if (name == "id" || name == "class" || name == "style") continue;
        if (name.find("style-") == 0) {
            apply_property(node.style, name.substr(6), value);
        }
    }

    // Recurse to children with this node's computed style as parent
    for (auto& child : node.children) {
        resolve_node(*child, sheet, &node.style);
    }
}

void StyleResolver::inherit_properties(UiComputedStyle& style, const UiComputedStyle& parent) {
    // CSS inherited properties: color, font-size, line-height, opacity
    style.color = parent.color;
    style.font_size = parent.font_size;
    style.line_height = parent.line_height;
    // Opacity compounds: child opacity is relative to parent
    style.opacity = parent.opacity;
}

bool StyleResolver::selector_matches(const UiNode& node, const StyleSelector& sel) const {
    if (!sel.id.empty() && node.id != sel.id) return false;
    if (!sel.class_.empty() && !node.has_class(sel.class_)) return false;
    if (!sel.tag.empty() && node.tag != sel.tag) return false;

    // Check pseudo-class against interaction state
    if (!sel.pseudo.empty()) {
        if (sel.pseudo == "hover" && !node.is_hovered()) return false;
        if (sel.pseudo == "active" && !node.is_active()) return false;
        if (sel.pseudo == "focus" && !node.is_focused()) return false;
    }

    if (!sel.parent.empty()) {
        bool found = false;
        for (auto* p = node.parent; p != nullptr; p = p->parent) {
            if (p->tag == sel.parent || p->has_class(sel.parent) || p->id == sel.parent) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

void StyleResolver::apply_property(UiComputedStyle& style, const std::string& name,
                                   const std::string& value) {
    // Display
    if (name == "display") {
        if (value == "flex") style.display = UiDisplay::flex;
        else if (value == "none") style.display = UiDisplay::none;
        return;
    }

    // Flex direction
    if (name == "flex-direction") {
        if (value == "row") style.flex_direction = UiFlexDirection::row;
        else if (value == "column") style.flex_direction = UiFlexDirection::column;
        else if (value == "row-reverse") style.flex_direction = UiFlexDirection::row_reverse;
        else if (value == "column-reverse") style.flex_direction = UiFlexDirection::column_reverse;
        return;
    }

    // Justify content
    if (name == "justify-content") {
        if (value == "flex-start") style.justify_content = UiJustify::flex_start;
        else if (value == "flex-end") style.justify_content = UiJustify::flex_end;
        else if (value == "center") style.justify_content = UiJustify::center;
        else if (value == "space-between") style.justify_content = UiJustify::space_between;
        else if (value == "space-around") style.justify_content = UiJustify::space_around;
        else if (value == "space-evenly") style.justify_content = UiJustify::space_evenly;
        return;
    }

    // Align items
    if (name == "align-items") {
        if (value == "flex-start") style.align_items = UiAlign::flex_start;
        else if (value == "flex-end") style.align_items = UiAlign::flex_end;
        else if (value == "center") style.align_items = UiAlign::center;
        else if (value == "stretch") style.align_items = UiAlign::stretch;
        else if (value == "baseline") style.align_items = UiAlign::baseline;
        return;
    }

    // Align self
    if (name == "align-self") {
        if (value == "auto") style.align_self = UiAlign::auto_;
        else if (value == "flex-start") style.align_self = UiAlign::flex_start;
        else if (value == "flex-end") style.align_self = UiAlign::flex_end;
        else if (value == "center") style.align_self = UiAlign::center;
        else if (value == "stretch") style.align_self = UiAlign::stretch;
        return;
    }

    // Flex wrap
    if (name == "flex-wrap") {
        if (value == "nowrap") style.flex_wrap = UiFlexWrap::no_wrap;
        else if (value == "wrap") style.flex_wrap = UiFlexWrap::wrap;
        else if (value == "wrap-reverse") style.flex_wrap = UiFlexWrap::wrap_reverse;
        return;
    }

    // Position
    if (name == "position") {
        if (value == "relative") style.position = UiPosition::relative;
        else if (value == "absolute") style.position = UiPosition::absolute;
        return;
    }

    // Overflow
    if (name == "overflow") {
        if (value == "visible") style.overflow = UiOverflow::visible;
        else if (value == "hidden") style.overflow = UiOverflow::hidden;
        else if (value == "scroll") style.overflow = UiOverflow::scroll;
        return;
    }

    // Flex grow / shrink
    if (name == "flex-grow") { style.flex_grow = parse_px(value); return; }
    if (name == "flex-shrink") { style.flex_shrink = parse_px(value); return; }

    // Dimensions
    if (name == "width") { style.width = parse_px(value); return; }
    if (name == "height") { style.height = parse_px(value); return; }
    if (name == "min-width") { style.min_width = parse_px(value); return; }
    if (name == "min-height") { style.min_height = parse_px(value); return; }
    if (name == "max-width") { style.max_width = parse_px(value); return; }
    if (name == "max-height") { style.max_height = parse_px(value); return; }

    // Margin (shorthand)
    if (name == "margin") { style.margin = parse_edges(value); return; }
    if (name == "margin-top") { style.margin.top = parse_px(value); return; }
    if (name == "margin-right") { style.margin.right = parse_px(value); return; }
    if (name == "margin-bottom") { style.margin.bottom = parse_px(value); return; }
    if (name == "margin-left") { style.margin.left = parse_px(value); return; }

    // Padding (shorthand)
    if (name == "padding") { style.padding = parse_edges(value); return; }
    if (name == "padding-top") { style.padding.top = parse_px(value); return; }
    if (name == "padding-right") { style.padding.right = parse_px(value); return; }
    if (name == "padding-bottom") { style.padding.bottom = parse_px(value); return; }
    if (name == "padding-left") { style.padding.left = parse_px(value); return; }

    // Position insets
    if (name == "top") { style.inset.top = parse_px(value); return; }
    if (name == "right") { style.inset.right = parse_px(value); return; }
    if (name == "bottom") { style.inset.bottom = parse_px(value); return; }
    if (name == "left") { style.inset.left = parse_px(value); return; }

    // Gap
    if (name == "gap") { style.row_gap = style.column_gap = parse_px(value); return; }
    if (name == "row-gap") { style.row_gap = parse_px(value); return; }
    if (name == "column-gap") { style.column_gap = parse_px(value); return; }

    // Background: gradient or solid color
    if (name == "background-color" || name == "background") {
        // Check for linear-gradient first
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
    if (name == "border") {
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

    // Border individual
    if (name == "border-color") { style.border_color = parse_color(value); return; }
    if (name == "border-width") { style.border_width = parse_px(value); return; }
    if (name == "border-radius") { style.border_radius = parse_corners(value); return; }

    // Opacity
    if (name == "opacity") { style.opacity = parse_px(value); return; }

    // Text
    if (name == "color") { style.color = parse_color(value); return; }
    if (name == "font-size") { style.font_size = parse_px(value); return; }
    if (name == "line-height") { style.line_height = parse_px(value); return; }

    // Transition: stored for UiAnimation to process
    if (name == "transition") { style.transition = value; return; }

    // Box shadow: "offset_x offset_y blur spread #color"
    // e.g. "0 4 8 0 rgba(0, 0, 0, 0.3)" or "2 2 6 #00000080"
    if (name == "box-shadow") {
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
    if (value == "auto") return -1.0f;
    float result = 0.0f;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}

UiEdges StyleResolver::parse_edges(const std::string& value) {
    std::vector<float> values;
    std::istringstream iss(value);
    std::string token;
    while (iss >> token) {
        values.push_back(parse_px(token));
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
