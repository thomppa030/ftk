#include "imgui_harness.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "ui/vfx_graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using fjell::ExprNodeType;
using fjell::ExpressionGraph;
using fjell::VfxEmitter;
using fjell::VfxGraph;
using fjell::module_node_id;
using ftk::test::ImGuiHarness;

namespace {

// A module with one float input, standing in for the real ones, whose
// code reaches the renderer.
struct RateModule final : fjell::VfxModule {
    float rate{50.0f};
    [[nodiscard]] fjell::ModuleType type() const override { return fjell::ModuleType::spawn_rate; }
    [[nodiscard]] const char* name() const override { return "Rate"; }
    uint32_t write_gpu_data(uint8_t*) const override { return 0; }
    [[nodiscard]] std::vector<fjell::PinDescriptor> pin_descriptors() override {
        return {{"rate", fjell::PinDataType::float_type, &rate, {}, 0.0f, 100.0f, 1.0f}};
    }
    void to_json(nlohmann::json&) const override {}
    void from_json(const nlohmann::json&) override {}
};

// Two rate modules (a float "rate" pin each) and, left of them, a
// Random node (a float out) and a Vec3 constant (a vec3 out).
struct Scene {
    VfxEmitter emitter;
    uint64_t random{0};
    uint64_t vec3{0};

    Scene() {
        for (int i = 0; i < 2; ++i) {
            auto module = std::make_unique<RateModule>();
            module->editor_x = 400.0f;
            module->editor_y = 40.0f + 160.0f * static_cast<float>(i);
            module->position_set = true;
            emitter.spawn_modules.push_back(std::move(module));
        }
        auto* r = emitter.expression_graph.add_node(ExprNodeType::random);
        r->editor_x = 60.0f;
        r->editor_y = 40.0f;
        random = r->node_id;
        auto* v = emitter.expression_graph.add_node(ExprNodeType::constant_vec3);
        v->editor_x = 60.0f;
        v->editor_y = 200.0f;
        vec3 = v->node_id;
    }

    [[nodiscard]] static uint64_t rate(int module) { return module_node_id(0, module) * 100; }
    [[nodiscard]] static uint64_t out(uint64_t node) { return node * 100 + 50; }
};

} // namespace

TEST_CASE("A link dragged from an output onto an input that takes it is made", "[ui][vfx_graph]") {
    Scene s;
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(2);

    h.drag(graph.pin_position(Scene::out(s.random)), graph.pin_position(Scene::rate(1)));
    REQUIRE(s.emitter.expression_graph.links.size() == 1);
    CHECK(s.emitter.expression_graph.links[0].source_pin_id == Scene::out(s.random));
    CHECK(s.emitter.expression_graph.links[0].target_pin_id == Scene::rate(1));
}

TEST_CASE("A vec3 can't be linked into a float", "[ui][vfx_graph]") {
    Scene s;
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(2);

    h.drag(graph.pin_position(Scene::out(s.vec3)), graph.pin_position(Scene::rate(0)));
    CHECK(s.emitter.expression_graph.links.empty());
}

TEST_CASE("A node dragged by its body keeps its place", "[ui][vfx_graph]") {
    Scene s;
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(2);

    // The first module's header, dragged down and right.
    const ImVec2 from = graph.screen_position({480.0f, 52.0f});
    const ImVec2 to = graph.screen_position({540.0f, 112.0f});
    h.drag(from, to);
    // Moved by (60, 60) to (460, 100), then onto the 12-unit grid.
    CHECK(s.emitter.spawn_modules[0]->editor_x == 456.0f);
    CHECK(s.emitter.spawn_modules[0]->editor_y == 96.0f);
    REQUIRE(graph.selected_module().has_value());
    CHECK(graph.selected_module()->second == 0);
}

TEST_CASE("Del removes the selected module and moves its links along", "[ui][vfx_graph]") {
    Scene s;
    s.emitter.expression_graph.add_link(Scene::out(s.random), Scene::rate(1));
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(2);

    const ImVec2 first = graph.screen_position({480.0f, 52.0f});
    h.drag(first, first);
    h.press(ImGuiKey_Delete);
    REQUIRE(s.emitter.spawn_modules.size() == 1);
    REQUIRE(s.emitter.expression_graph.links.size() == 1);
    CHECK(s.emitter.expression_graph.links[0].target_pin_id == Scene::rate(0));
}

namespace {

// The middle of the first module's rate field: at the right of its row.
glm::vec2 rate_field() {
    return {400.0f + 176.0f - 12.0f - 31.0f, 40.0f + 26.0f + 11.0f};
}

} // namespace

TEST_CASE("A value dragged sideways in its node changes the module", "[ui][vfx_graph]") {
    Scene s;
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(3);

    const ImVec2 at = graph.screen_position(rate_field());
    h.drag(at, {at.x + 20.0f, at.y});
    auto& module = static_cast<RateModule&>(*s.emitter.spawn_modules[0]);
    CHECK(module.rate == 70.0f);   // 50, one a pixel
    CHECK(module.editor_x == 400.0f);   // the field took the drag, not the node
}

TEST_CASE("A value double-clicked in its node is typed", "[ui][vfx_graph]") {
    Scene s;
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(3);

    const ImVec2 at = graph.screen_position(rate_field());
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    h.step();
    for (int i = 0; i < 2; ++i) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        h.step();
    }
    h.step(2);
    h.type("12");
    h.press(ImGuiKey_Enter);
    h.step();
    CHECK(static_cast<RateModule&>(*s.emitter.spawn_modules[0]).rate == 12.0f);
}

namespace {

// A module whose "extra" value is only used in one mode, and whose "fixed"
// value is taken over by another setting: shown then, not editable.
struct ModalModule final : fjell::VfxModule {
    bool cone{false};
    float spread{1.0f};
    float fixed{2.0f};
    [[nodiscard]] fjell::ModuleType type() const override { return fjell::ModuleType::initial_velocity; }
    [[nodiscard]] const char* name() const override { return "Modal"; }
    uint32_t write_gpu_data(uint8_t*) const override { return 0; }
    [[nodiscard]] std::vector<fjell::PinDescriptor> pin_descriptors() override {
        return {{.name = "spread", .data_type = fjell::PinDataType::float_type, .value_ptr = &spread, .shown = cone},
                {.name = "fixed", .data_type = fjell::PinDataType::float_type, .value_ptr = &fixed, .disabled = true}};
    }
    void to_json(nlohmann::json&) const override {}
    void from_json(const nlohmann::json&) override {}
};

} // namespace

TEST_CASE("A value its module's mode leaves unused has no pin, unless linked", "[ui][vfx_graph]") {
    Scene s;
    s.emitter.update_modules.push_back(std::make_unique<ModalModule>());
    auto& modal = static_cast<ModalModule&>(*s.emitter.update_modules[0]);
    const uint64_t spread = module_node_id(1, 0) * 100;
    const uint64_t fixed = spread + 1;
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(2);

    const ImVec2 none{};
    CHECK(graph.pin_position(spread).x == none.x);   // not there
    CHECK(graph.pin_position(fixed).x != none.x);

    modal.cone = true;
    h.step();
    CHECK(graph.pin_position(spread).x != none.x);

    // Hidden again, but a link runs into it: it stays, so the link is seen.
    s.emitter.expression_graph.add_link(Scene::out(s.random), spread);
    modal.cone = false;
    h.step();
    CHECK(graph.pin_position(spread).x != none.x);
}

TEST_CASE("A value another setting takes over keeps its pin but no field", "[ui][vfx_graph]") {
    Scene s;
    auto modal = std::make_unique<ModalModule>();
    modal->editor_x = 400.0f;
    modal->editor_y = 400.0f;
    modal->position_set = true;
    s.emitter.update_modules.push_back(std::move(modal));
    VfxGraph graph;
    ImGuiHarness h;
    h.set_ui([&] { (void)graph.draw(s.emitter); });
    h.step(3);

    // Where its field would be, at the right of the pin's row: dragging there
    // moves the node instead of the value.
    const uint64_t fixed = module_node_id(1, 0) * 100 + 1;
    const ImVec2 pin = graph.pin_position(fixed);
    const ImVec2 at{graph.screen_position({400.0f + 176.0f - 12.0f - 31.0f, 0.0f}).x, pin.y};
    h.drag(at, {at.x + 20.0f, at.y});
    CHECK(static_cast<ModalModule&>(*s.emitter.update_modules[0]).fixed == 2.0f);
}
