#include "ui/node_editor_panel.hpp"
#include "scene/scene.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imnodes.h>

#include <algorithm>

namespace fjell {

int NodeEditorPanel::next_id() {
    return ++id_counter_;
}

void NodeEditorPanel::init(Scene* scene, uint32_t* selected_id) {
    scene_ = scene;
    selected_id_ = selected_id;
    ImNodes::CreateContext();

    ImNodesStyle& style = ImNodes::GetStyle();
    style.Flags |= ImNodesStyleFlags_GridSnapping;

    // Match Fjell dark theme
    style.Colors[ImNodesCol_NodeBackground]         = IM_COL32(43, 45, 49, 255);   // #2b2d31
    style.Colors[ImNodesCol_NodeBackgroundHovered]   = IM_COL32(49, 51, 56, 255);   // #313338
    style.Colors[ImNodesCol_NodeBackgroundSelected]  = IM_COL32(56, 58, 64, 255);   // #383a40
    style.Colors[ImNodesCol_TitleBar]                = IM_COL32(30, 31, 34, 255);   // #1e1f22
    style.Colors[ImNodesCol_TitleBarHovered]         = IM_COL32(49, 51, 56, 255);
    style.Colors[ImNodesCol_TitleBarSelected]        = IM_COL32(212, 160, 84, 255); // #D4A054
    style.Colors[ImNodesCol_Link]                    = IM_COL32(212, 160, 84, 200);
    style.Colors[ImNodesCol_LinkHovered]             = IM_COL32(212, 160, 84, 255);
    style.Colors[ImNodesCol_LinkSelected]            = IM_COL32(184, 137, 62, 255); // #B8893E
    style.Colors[ImNodesCol_Pin]                     = IM_COL32(212, 160, 84, 200);
    style.Colors[ImNodesCol_PinHovered]              = IM_COL32(212, 160, 84, 255);
    style.Colors[ImNodesCol_GridBackground]          = IM_COL32(17, 18, 20, 255);   // #111214
    style.Colors[ImNodesCol_GridLine]                = IM_COL32(43, 45, 49, 100);
    style.Colors[ImNodesCol_GridLinePrimary]         = IM_COL32(43, 45, 49, 180);

    style.NodeCornerRounding = 6.0f;
    style.NodePadding        = ImVec2(10.0f, 6.0f);
    style.LinkThickness      = 2.5f;
    style.PinCircleRadius    = 4.5f;

    // Reserve to prevent reallocation / dangling refs
    nodes_.reserve(16);

    // Create default graph: Emitter → Output
    create_node(NodeType::emitter, 50.0f, 100.0f);
    create_node(NodeType::velocity, 50.0f, 300.0f);
    create_node(NodeType::color_over_life, 350.0f, 100.0f);
    create_node(NodeType::size_over_life, 350.0f, 300.0f);
    create_node(NodeType::gravity, 350.0f, 480.0f);
    create_node(NodeType::output, 650.0f, 200.0f);

    // Wire defaults: nodes[0..4] outputs → nodes[5] (output) inputs
    auto& out_node = nodes_[5];
    auto link = [&](int from, int to) {
        links_.push_back({next_id(), from, to});
    };
    link(nodes_[0].outputs[0].id, out_node.inputs[0].id);
    link(nodes_[1].outputs[0].id, out_node.inputs[1].id);
    link(nodes_[2].outputs[0].id, out_node.inputs[2].id);
    link(nodes_[3].outputs[0].id, out_node.inputs[3].id);
    link(nodes_[4].outputs[0].id, out_node.inputs[4].id);

    initialized_ = true;
}

void NodeEditorPanel::shutdown() {
    if (initialized_) {
        ImNodes::DestroyContext();
        initialized_ = false;
    }
}

Node& NodeEditorPanel::create_node(NodeType type, float x, float y) {
    Node node;
    node.id = next_id();
    node.type = type;

    switch (type) {
    case NodeType::emitter:
        node.title = "Emitter";
        node.outputs.push_back({next_id(), "Emitter"});
        break;
    case NodeType::velocity:
        node.title = "Velocity";
        node.outputs.push_back({next_id(), "Velocity"});
        break;
    case NodeType::color_over_life:
        node.title = "Color Over Life";
        node.outputs.push_back({next_id(), "Color"});
        break;
    case NodeType::size_over_life:
        node.title = "Size Over Life";
        node.outputs.push_back({next_id(), "Size"});
        break;
    case NodeType::gravity:
        node.title = "Gravity";
        node.outputs.push_back({next_id(), "Force"});
        break;
    case NodeType::output:
        node.title = "Particle Output";
        node.inputs.push_back({next_id(), "Emitter"});
        node.inputs.push_back({next_id(), "Velocity"});
        node.inputs.push_back({next_id(), "Color"});
        node.inputs.push_back({next_id(), "Size"});
        node.inputs.push_back({next_id(), "Force"});
        break;
    }

    pending_positions_[node.id] = ImVec2(x, y);
    nodes_.push_back(std::move(node));
    return nodes_.back();
}

void NodeEditorPanel::draw_node(Node& node) {
    ImNodes::BeginNode(node.id);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(node.title.c_str());
    ImNodes::EndNodeTitleBar();

    // Draw input pins
    for (auto& pin : node.inputs) {
        ImNodes::BeginInputAttribute(pin.id);
        ImGui::TextUnformatted(pin.name.c_str());
        ImNodes::EndInputAttribute();
    }

    // Draw node-specific controls
    ImGui::PushItemWidth(140.0f);
    switch (node.type) {
    case NodeType::emitter:
        ImGui::DragFloat("Rate", &node.emission_rate, 1.0f, 1.0f, 100000.0f);
        ImGui::DragFloat("Life Min", &node.lifetime_min, 0.05f, 0.01f, 30.0f);
        ImGui::DragFloat("Life Max", &node.lifetime_max, 0.05f, 0.01f, 30.0f);
        {
            int mp = static_cast<int>(node.max_particles);
            if (ImGui::DragInt("Max", &mp, 100.0f, 1, 1000000))
                node.max_particles = static_cast<uint32_t>(mp);
        }
        {
            const char* modes[] = {"Additive", "Alpha"};
            int current = static_cast<int>(node.blend_mode);
            if (ImGui::Combo("Blend", &current, modes, 2)) {
                node.blend_mode = static_cast<ParticleBlendMode>(current);
            }
        }
        break;
    case NodeType::velocity:
        ImGui::DragFloat3("Min", &node.velocity_min.x, 0.1f);
        ImGui::DragFloat3("Max", &node.velocity_max.x, 0.1f);
        break;
    case NodeType::color_over_life:
        ImGui::ColorEdit4("Start", &node.color_start.x);
        ImGui::ColorEdit4("End", &node.color_end.x);
        break;
    case NodeType::size_over_life:
        ImGui::DragFloat("Start", &node.size_start, 0.005f, 0.001f, 10.0f);
        ImGui::DragFloat("End", &node.size_end, 0.005f, 0.001f, 10.0f);
        break;
    case NodeType::gravity:
        ImGui::DragFloat3("Force", &node.gravity.x, 0.1f);
        break;
    case NodeType::output:
        break;
    }
    ImGui::PopItemWidth();

    // Draw output pins
    for (auto& pin : node.outputs) {
        ImNodes::BeginOutputAttribute(pin.id);
        ImGui::Indent(100.0f);
        ImGui::TextUnformatted(pin.name.c_str());
        ImNodes::EndOutputAttribute();
    }

    ImNodes::EndNode();
}

Node* NodeEditorPanel::find_node(NodeType type) {
    for (auto& n : nodes_) {
        if (n.type == type) return &n;
    }
    return nullptr;
}

void NodeEditorPanel::load_from_emitter(const ParticleEmitter& e) {
    if (auto* n = find_node(NodeType::emitter)) {
        n->emission_rate = e.emission_rate;
        n->lifetime_min  = e.lifetime_min;
        n->lifetime_max  = e.lifetime_max;
        n->max_particles = e.max_particles;
        n->blend_mode    = e.blend_mode;
    }
    if (auto* n = find_node(NodeType::velocity)) {
        n->velocity_min = e.velocity_min;
        n->velocity_max = e.velocity_max;
    }
    if (auto* n = find_node(NodeType::color_over_life)) {
        n->color_start = e.color_start;
        n->color_end   = e.color_end;
    }
    if (auto* n = find_node(NodeType::size_over_life)) {
        n->size_start = e.size_start;
        n->size_end   = e.size_end;
    }
    if (auto* n = find_node(NodeType::gravity)) {
        n->gravity = e.gravity;
    }
}

void NodeEditorPanel::apply_to_selected() {
    if (!scene_ || !selected_id_ || *selected_id_ == 0) return;
    auto* obj = scene_->find(*selected_id_);
    if (!obj) return;

    ParticleEmitter built = build_emitter();
    if (!obj->emitter) {
        obj->emitter = built;
    } else {
        // Preserve runtime accumulator
        float acc = obj->emitter->emit_accumulator;
        obj->emitter = built;
        obj->emitter->emit_accumulator = acc;
    }
    scene_->mark_dirty();
}

void NodeEditorPanel::draw() {
    if (!initialized_) return;

    if (ImGui::Begin("VFX Graph")) {
        // Toolbar
        {
            bool has_selection = selected_id_ && *selected_id_ != 0;
            RenderObject* obj = has_selection ? scene_->find(*selected_id_) : nullptr;
            bool has_emitter = obj && obj->emitter.has_value();

            if (ImGui::Button("Apply to Selected") && obj) {
                apply_to_selected();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(has_selection
                    ? "Apply graph to selected object's emitter"
                    : "Select an object first");
            }

            ImGui::SameLine();
            if (ImGui::Button("Load from Selected") && has_emitter) {
                load_from_emitter(*obj->emitter);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(has_emitter
                    ? "Load selected object's emitter into graph"
                    : "Selected object has no emitter");
            }

            ImGui::SameLine();
            ImGui::Checkbox("Auto Apply", &auto_apply_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Continuously apply graph changes to selected object");
            }

            ImGui::Separator();
        }

        // Right-click context menu for adding nodes
        if (ImGui::BeginPopupContextWindow("##node_ctx")) {
            ImVec2 click_pos = ImGui::GetMousePosOnOpeningCurrentPopup();

            if (ImGui::MenuItem("Emitter"))        create_node(NodeType::emitter, click_pos.x, click_pos.y);
            if (ImGui::MenuItem("Velocity"))        create_node(NodeType::velocity, click_pos.x, click_pos.y);
            if (ImGui::MenuItem("Color Over Life")) create_node(NodeType::color_over_life, click_pos.x, click_pos.y);
            if (ImGui::MenuItem("Size Over Life"))  create_node(NodeType::size_over_life, click_pos.x, click_pos.y);
            if (ImGui::MenuItem("Gravity"))         create_node(NodeType::gravity, click_pos.x, click_pos.y);
            if (ImGui::MenuItem("Output"))          create_node(NodeType::output, click_pos.x, click_pos.y);
            ImGui::EndPopup();
        }

        ImNodes::BeginNodeEditor();

        // Apply deferred positions (must happen inside BeginNodeEditor)
        for (auto& [node_id, pos] : pending_positions_) {
            ImNodes::SetNodeEditorSpacePos(node_id, pos);
        }
        pending_positions_.clear();

        for (auto& node : nodes_) {
            draw_node(node);
        }

        for (auto& link : links_) {
            ImNodes::Link(link.id, link.start_pin, link.end_pin);
        }

        ImNodes::EndNodeEditor();

        // Handle new links
        int start_pin = 0, end_pin = 0;
        if (ImNodes::IsLinkCreated(&start_pin, &end_pin)) {
            links_.push_back({next_id(), start_pin, end_pin});
        }

        // Handle link deletion
        int destroyed_link = 0;
        if (ImNodes::IsLinkDestroyed(&destroyed_link)) {
            auto it = std::find_if(links_.begin(), links_.end(),
                [destroyed_link](const Link& l) { return l.id == destroyed_link; });
            if (it != links_.end()) links_.erase(it);
        }

        // Delete selected nodes/links with Delete key
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            int num_selected_links = ImNodes::NumSelectedLinks();
            if (num_selected_links > 0) {
                std::vector<int> selected(static_cast<size_t>(num_selected_links));
                ImNodes::GetSelectedLinks(selected.data());
                for (int link_id : selected) {
                    auto it = std::find_if(links_.begin(), links_.end(),
                        [link_id](const Link& l) { return l.id == link_id; });
                    if (it != links_.end()) links_.erase(it);
                }
            }

            int num_selected_nodes = ImNodes::NumSelectedNodes();
            if (num_selected_nodes > 0) {
                std::vector<int> selected(static_cast<size_t>(num_selected_nodes));
                ImNodes::GetSelectedNodes(selected.data());
                for (int node_id : selected) {
                    // Remove links connected to this node
                    auto& n = *std::find_if(nodes_.begin(), nodes_.end(),
                        [node_id](const Node& nd) { return nd.id == node_id; });
                    auto is_connected = [&](const Link& l) {
                        for (auto& p : n.inputs)
                            if (p.id == l.start_pin || p.id == l.end_pin) return true;
                        for (auto& p : n.outputs)
                            if (p.id == l.start_pin || p.id == l.end_pin) return true;
                        return false;
                    };
                    std::erase_if(links_, is_connected);
                    std::erase_if(nodes_, [node_id](const Node& nd) { return nd.id == node_id; });
                }
            }
        }

        // Auto-apply: push graph state to selected object every frame
        if (auto_apply_) {
            apply_to_selected();
        }
    }
    ImGui::End();
}

ParticleEmitter NodeEditorPanel::build_emitter() const {
    ParticleEmitter e{};

    // Find output node and trace back through links
    const Node* output = nullptr;
    for (auto& n : nodes_) {
        if (n.type == NodeType::output) {
            output = &n;
            break;
        }
    }
    if (!output) return e;

    auto find_connected_node = [&](int input_pin_id) -> const Node* {
        for (auto& link : links_) {
            if (link.end_pin == input_pin_id) {
                int out_pin = link.start_pin;
                for (auto& n : nodes_) {
                    for (auto& p : n.outputs) {
                        if (p.id == out_pin) return &n;
                    }
                }
            }
        }
        return nullptr;
    };

    // Emitter input (index 0)
    if (output->inputs.size() > 0) {
        if (auto* n = find_connected_node(output->inputs[0].id)) {
            e.emission_rate  = n->emission_rate;
            e.lifetime_min   = n->lifetime_min;
            e.lifetime_max   = n->lifetime_max;
            e.max_particles  = n->max_particles;
            e.blend_mode     = n->blend_mode;
        }
    }

    // Velocity input (index 1)
    if (output->inputs.size() > 1) {
        if (auto* n = find_connected_node(output->inputs[1].id)) {
            e.velocity_min = n->velocity_min;
            e.velocity_max = n->velocity_max;
        }
    }

    // Color input (index 2)
    if (output->inputs.size() > 2) {
        if (auto* n = find_connected_node(output->inputs[2].id)) {
            e.color_start = n->color_start;
            e.color_end   = n->color_end;
        }
    }

    // Size input (index 3)
    if (output->inputs.size() > 3) {
        if (auto* n = find_connected_node(output->inputs[3].id)) {
            e.size_start = n->size_start;
            e.size_end   = n->size_end;
        }
    }

    // Gravity input (index 4)
    if (output->inputs.size() > 4) {
        if (auto* n = find_connected_node(output->inputs[4].id)) {
            e.gravity = n->gravity;
        }
    }

    return e;
}

} // namespace fjell
