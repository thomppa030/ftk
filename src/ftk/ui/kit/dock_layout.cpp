#include "ftk/ui/kit/dock.hpp"

#include <nlohmann/json.hpp>

#include <utility>

// The dock tree's own parts, which neither kit draws: its nodes made by
// hand, and named layouts read and written as JSON.

namespace ftk::ui {

namespace {

nlohmann::json to_json(const DockNode& node) {
    nlohmann::json out = nlohmann::json::object();
    if (node.split != DockAxis::None && node.children.size() == 2) {
        out["split"] = node.split == DockAxis::LeftRight ? "x" : "y";
        out["ratio"] = node.ratio;
        out["children"] = nlohmann::json::array({to_json(node.children[0]), to_json(node.children[1])});
    }
    if (!node.windows.empty()) out["windows"] = node.windows;
    return out;
}

std::optional<DockNode> from_json(const nlohmann::json& in) {
    if (!in.is_object()) return std::nullopt;
    DockNode node;
    if (const auto split = in.find("split"); split != in.end()) {
        if (!split->is_string()) return std::nullopt;
        const auto& axis = split->get_ref<const std::string&>();
        if (axis == "x") {
            node.split = DockAxis::LeftRight;
        } else if (axis == "y") {
            node.split = DockAxis::TopBottom;
        } else {
            return std::nullopt;
        }
        if (const auto ratio = in.find("ratio"); ratio != in.end() && ratio->is_number()) {
            node.ratio = ratio->get<float>();
        }
        const auto children = in.find("children");
        if (children == in.end() || !children->is_array() || children->size() != 2) return std::nullopt;
        for (const nlohmann::json& child : *children) {
            auto read = from_json(child);
            if (!read) return std::nullopt;
            node.children.push_back(std::move(*read));
        }
    }
    if (const auto windows = in.find("windows"); windows != in.end()) {
        if (!windows->is_array()) return std::nullopt;
        for (const nlohmann::json& window : *windows) {
            if (!window.is_string()) return std::nullopt;
            node.windows.push_back(window.get<std::string>());
        }
    }
    return node;
}

} // namespace

DockNode DockNode::left_right(float ratio, DockNode left, DockNode right) {
    DockNode node{.split = DockAxis::LeftRight, .ratio = ratio};
    node.children.push_back(std::move(left));
    node.children.push_back(std::move(right));
    return node;
}

DockNode DockNode::top_bottom(float ratio, DockNode top, DockNode bottom) {
    DockNode node{.split = DockAxis::TopBottom, .ratio = ratio};
    node.children.push_back(std::move(top));
    node.children.push_back(std::move(bottom));
    return node;
}

DockNode DockNode::tabs(std::initializer_list<std::string> windows) {
    return {.windows = windows};
}

std::string dock_layout_json(const DockLayout& layout) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [name, tree] : layout) out[name] = to_json(tree);
    return out.dump(2);
}

std::optional<DockLayout> dock_layout_from_json(std::string_view json) {
    const nlohmann::json in = nlohmann::json::parse(json, nullptr, false);
    if (in.is_discarded() || !in.is_object()) return std::nullopt;
    DockLayout layout;
    for (const auto& [name, tree] : in.items()) {
        auto read = from_json(tree);
        if (!read) return std::nullopt;
        layout.emplace(name, std::move(*read));
    }
    return layout;
}

} // namespace ftk::ui
