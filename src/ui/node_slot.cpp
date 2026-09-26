#include "ui/node_slot.hpp"

#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/edit_record.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/grouped_picker.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/slot.hpp"
#include "ui/node_visuals.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <system_error>
#include <string>
#include <vector>

namespace fjell {

namespace {

/// The one node a hierarchy drag carries, or 0 when it carries several.
uint32_t payload_node(const ImGuiPayload& payload) {
    if (static_cast<std::size_t>(payload.DataSize) != sizeof(uint32_t)) return 0;
    uint32_t id = 0;
    std::memcpy(&id, payload.Data, sizeof(id));
    return id;
}

/// Why the payload's nodes cannot go in the slot, or empty when they can.
std::string drop_refusal(const NodeSlotConfig& config, const ImGuiPayload& payload) {
    const auto count = static_cast<std::size_t>(payload.DataSize) / sizeof(uint32_t);
    if (count != 1) return "Takes one node; " + std::to_string(count) + " are dragged.";
    const Node* node = config.scene->find(payload_node(payload));
    if (node == nullptr) return "That node is not in this scene.";
    return config.refusal ? config.refusal(*node) : std::string();
}

/// What the field shows: the node as the hierarchy draws it, the empty text
/// while it holds nothing, and a node that is gone in the error colour, never
/// as empty. `text` holds the characters the face points at.
ui::SlotFace describe(const NodeSlotConfig& config, uint32_t node, const Node* held, std::string& text) {
    ui::SlotFace face;
    if (node == 0) {
        face.glyph = config.empty_glyph != nullptr ? config.empty_glyph : ui::icon::transform;
        face.glyph_colour = theme::text_disabled();
        text = config.empty_text;
        face.text_colour = theme::text_secondary();
    } else if (held == nullptr) {
        face.glyph = ui::icon::warning;
        face.glyph_colour = theme::error();
        text = "missing: the node is gone";
        face.text_colour = theme::error();
    } else {
        const NodeVisuals visuals = node_visuals(*held);
        face.glyph = visuals.icon;
        face.glyph_colour = visuals.tint       ? *visuals.tint
                            : visuals.category ? theme::category(*visuals.category)
                                               : theme::text_secondary();
        text = held->name;
        face.text_colour = theme::text();
    }
    face.text = text;
    return face;
}

/// Every node the slot takes, in scene order, for its picker.
std::vector<ui::grouped_picker::Group> gather(const NodeSlotConfig& config) {
    std::vector<ui::grouped_picker::Group> groups(1);
    for (const auto& node : config.scene->objects()) {
        if (config.refusal && !config.refusal(*node).empty()) continue;
        std::string path = node_parent_path(*config.scene, *node);
        groups.front().items.push_back({.value = std::to_string(node->id),
                                        .label = node->name,
                                        .search_text = path,
                                        .detail = std::move(path)});
    }
    return groups;
}

} // namespace

std::string node_parent_path(const Scene& scene, const Node& node) {
    std::vector<const Node*> chain;
    for (const Node* up = node.parent_id >= 0 ? scene.find(static_cast<uint32_t>(node.parent_id)) : nullptr;
         up != nullptr && chain.size() < scene.objects().size();
         up = up->parent_id >= 0 ? scene.find(static_cast<uint32_t>(up->parent_id)) : nullptr) {
        chain.push_back(up);
    }
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!path.empty()) path += " / ";
        path += (*it)->name;
    }
    return path;
}

bool node_slot(const NodeSlotConfig& config, uint32_t& node) {
    bool changed = false;
    const Scene* scene = config.scene;
    auto name_of = [&](uint32_t id) -> std::string {
        if (id == 0) return std::string(config.empty_text);
        const Node* found = scene != nullptr ? scene->find(id) : nullptr;
        return found != nullptr ? found->name : std::string("missing");
    };
    const std::string name_before = name_of(node);

    ImGui::PushID(config.id);
    ui::Slot slot(scene != nullptr ? NODES_PAYLOAD : nullptr,
                  [&](const ImGuiPayload& payload) { return drop_refusal(config, payload); });
    if (slot.clicked()) ui::grouped_picker::open(config.id);
    const Node* held = node != 0 && scene != nullptr ? scene->find(node) : nullptr;
    if (slot.hovered() && held != nullptr) {
        const std::string path = node_parent_path(*scene, *held);
        ImGui::SetItemTooltip("%s", path.empty() ? held->name.c_str() : (path + " / " + held->name).c_str());
    }
    if (const ImGuiPayload* dropped = slot.dropped()) {
        node = payload_node(*dropped);
        changed = true;
    }

    // The picker hangs under the field, so it draws right after it. Its list
    // is a walk over the scene, taken only while it is open.
    if (scene != nullptr && ui::grouped_picker::is_open(config.id)) {
        const auto groups = gather(config);
        std::string picked;
        if (ui::grouped_picker::draw(config.id, groups,
                                     {.empty_message = "No node in the scene fits.", .hide_group_headers = true},
                                     picked)) {
            uint32_t id = 0;
            if (std::from_chars(picked.data(), picked.data() + picked.size(), id).ec == std::errc{}) {
                node = id;
                changed = true;
            }
        }
    }

    held = node != 0 && scene != nullptr ? scene->find(node) : nullptr;
    std::string text;
    const ui::SlotFace face = describe(config, node, held, text);
    // A node that was taken and has since stopped fitting: a script removed
    // from it, say.
    const std::string problem = held != nullptr && config.refusal ? config.refusal(*held) : std::string();
    if (slot.finish(face, node == 0, problem)) {
        node = 0;
        changed = true;
    }

    ImGui::PopID();
    if (changed) {
        ui::detail::track_field(ImGui::GetID(config.id), name_before, name_of(node), false, {true, true});
    }
    return changed;
}

} // namespace fjell
