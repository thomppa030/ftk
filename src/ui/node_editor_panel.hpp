#pragma once

#include "renderer/particle_emitter.hpp"

#include <imgui.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Scene;

enum class NodeType : uint8_t {
    emitter,
    velocity,
    color_over_life,
    size_over_life,
    gravity,
    output,
};

struct NodePin {
    int id;
    std::string name;
};

struct VfxNode {
    int id;
    NodeType type;
    std::string title;
    std::vector<NodePin> inputs;
    std::vector<NodePin> outputs;

    // Per-type data
    float emission_rate{50.0f};
    float lifetime_min{1.0f};
    float lifetime_max{3.0f};
    uint32_t max_particles{10000};

    glm::vec3 velocity_min{-0.5f, 1.0f, -0.5f};
    glm::vec3 velocity_max{0.5f, 3.0f, 0.5f};

    glm::vec4 color_start{1.0f, 0.8f, 0.2f, 1.0f};
    glm::vec4 color_end{1.0f, 0.1f, 0.0f, 0.0f};

    float size_start{0.1f};
    float size_end{0.02f};

    glm::vec3 gravity{0.0f, -9.81f, 0.0f};

    ParticleBlendMode blend_mode{ParticleBlendMode::additive};
};

struct Link {
    int id;
    int start_pin;
    int end_pin;
};

class NodeEditorPanel {
public:
    void init(Scene* scene, uint32_t* selected_id);
    void shutdown();
    void draw();

    void set_visible(bool v) { visible_ = v; }
    [[nodiscard]] bool visible() const { return visible_; }

    [[nodiscard]] EmitterComponent build_emitter() const;
    void load_from_emitter(const EmitterComponent& e);

private:
    int next_id();
    VfxNode& create_node(NodeType type, float x, float y);
    void draw_node(VfxNode& node);
    void apply_to_selected();

    VfxNode* find_node(NodeType type);

    Scene* scene_{nullptr};
    uint32_t* selected_id_{nullptr};

    std::vector<VfxNode> nodes_;
    std::vector<Link> links_;
    std::unordered_map<int, ImVec2> pending_positions_;
    int id_counter_{0};
    bool initialized_{false};
    bool auto_apply_{false};
    bool visible_{false};
};

} // namespace fjell
