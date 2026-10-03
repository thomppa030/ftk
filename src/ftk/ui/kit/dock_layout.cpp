#include "ftk/ui/kit/dock.hpp"

#include <nlohmann/json.hpp>

#include <glm/vec2.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>

// The dock tree's own parts, which neither kit draws: its nodes made by
// hand, named layouts read and written as JSON, and the trees ImGui kept in
// its settings file read back.

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

namespace {

// A node of ImGui's: its split, its size when it was saved, its two parts
// in order, and the windows docked in it with their places among its tabs.
struct ImGuiNode {
    DockAxis split{DockAxis::None};
    glm::vec2 size{0.0f};
    std::vector<uint32_t> children{};
    std::vector<std::pair<int, std::string>> windows{};
};

// "0x0592BDEE" as its number; 0 when it isn't one.
uint32_t hex_of(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    uint32_t value = 0;
    const auto read = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return read.ec == std::errc{} ? value : 0;
}

// "2209,1302" as two numbers.
glm::vec2 size_of(std::string_view text) {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) return glm::vec2{0.0f};
    int x = 0;
    int y = 0;
    (void)std::from_chars(text.data(), text.data() + comma, x);
    (void)std::from_chars(text.data() + comma + 1, text.data() + text.size(), y);
    return {static_cast<float>(x), static_cast<float>(y)};
}

// The words of a line, between its spaces.
std::vector<std::string_view> words_of(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < line.size()) {
        std::size_t end = line.find(' ', at);
        if (end == std::string_view::npos) end = line.size();
        if (end > at) words.push_back(line.substr(at, end - at));
        at = end + 1;
    }
    return words;
}

DockNode tree_of(const std::map<uint32_t, ImGuiNode>& nodes, uint32_t id) {
    DockNode out;
    const auto found = nodes.find(id);
    if (found == nodes.end()) return out;
    const ImGuiNode& node = found->second;
    if (node.split != DockAxis::None && node.children.size() == 2) {
        // The first part's share of the two as they were saved.
        const int axis = node.split == DockAxis::LeftRight ? 0 : 1;
        const auto first = nodes.find(node.children[0]);
        const auto second = nodes.find(node.children[1]);
        const float a = first != nodes.end() ? first->second.size[axis] : 0.0f;
        const float b = second != nodes.end() ? second->second.size[axis] : 0.0f;
        out.split = node.split;
        out.ratio = a + b > 0.0f ? a / (a + b) : 0.5f;
        out.children.push_back(tree_of(nodes, node.children[0]));
        out.children.push_back(tree_of(nodes, node.children[1]));
        return out;
    }
    std::vector<std::pair<int, std::string>> windows = node.windows;
    std::ranges::stable_sort(windows, {}, &std::pair<int, std::string>::first);
    for (auto& [order, name] : windows) out.windows.push_back(std::move(name));
    return out;
}

} // namespace

std::vector<DockNode> dock_trees_from_imgui_ini(std::string_view ini) {
    std::map<uint32_t, ImGuiNode> nodes;
    std::vector<uint32_t> spaces;
    // Where each window is docked, as its [Window] entry says: placed once
    // every node is read, as the nodes come after the windows.
    std::vector<std::tuple<uint32_t, int, std::string>> docked;
    std::string_view type;
    std::string_view entry;
    while (!ini.empty()) {
        const std::size_t end = ini.find('\n');
        std::string_view line = ini.substr(0, end);
        ini.remove_prefix(end == std::string_view::npos ? ini.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty()) continue;
        // [type][entry] begins an entry; the name runs to the last ].
        if (line.front() == '[' && line.back() == ']') {
            const std::size_t close = line.find(']');
            type = line.substr(1, close - 1);
            entry = close + 2 < line.size() && line[close + 1] == '['
                        ? line.substr(close + 2, line.size() - close - 3)
                        : std::string_view{};
            continue;
        }
        // "DockId=0x00000014,1": the node, and its place among the tabs.
        if (type == "Window" && line.starts_with("DockId=")) {
            const std::string_view value = line.substr(7);
            const std::size_t comma = value.find(',');
            int order = 0;
            if (comma != std::string_view::npos) {
                (void)std::from_chars(value.data() + comma + 1, value.data() + value.size(), order);
            }
            docked.emplace_back(hex_of(value.substr(0, comma)), order, std::string(entry));
            continue;
        }
        if (type != "Docking" || entry != "Data") continue;
        // "DockNode ID=0x2B Parent=0x29 SizeRef=411,651 Split=Y"
        const std::vector<std::string_view> words = words_of(line);
        if (words.empty()) continue;
        uint32_t id = 0;
        uint32_t parent = 0;
        ImGuiNode read;
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::size_t equals = words[i].find('=');
            if (equals == std::string_view::npos) continue;
            const std::string_view key = words[i].substr(0, equals);
            const std::string_view value = words[i].substr(equals + 1);
            if (key == "ID") {
                id = hex_of(value);
            } else if (key == "Parent") {
                parent = hex_of(value);
            } else if (key == "SizeRef" || (key == "Size" && read.size == glm::vec2{0.0f})) {
                read.size = size_of(value);
            } else if (key == "Split") {
                read.split = value == "X" ? DockAxis::LeftRight : DockAxis::TopBottom;
            }
        }
        if (id == 0) continue;
        ImGuiNode& node = nodes[id];
        node.split = read.split;
        node.size = read.size;
        if (words[0] == "DockSpace") {
            spaces.push_back(id);
        } else if (parent != 0) {
            nodes[parent].children.push_back(id);
        }
    }
    for (auto& [id, order, name] : docked) {
        if (const auto node = nodes.find(id); node != nodes.end()) node->second.windows.emplace_back(order, std::move(name));
    }
    std::vector<DockNode> trees;
    trees.reserve(spaces.size());
    for (const uint32_t space : spaces) trees.push_back(tree_of(nodes, space));
    return trees;
}

} // namespace ftk::ui
