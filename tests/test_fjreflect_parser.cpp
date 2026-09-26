#include "fjreflect/parser.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Parses a header written to a temporary file, the way fjreflect reads a
// project's scripts.
fjreflect::ParseResult parse_header(const std::string& name, const std::string& text) {
    auto path = fs::temp_directory_path() /
                ("fjreflect_test_" + std::to_string(fjell::platform::process_id()) + "_" + name);
    std::ofstream(path) << text;
    auto result = fjreflect::parse_file(path.string());
    fs::remove(path);
    return result;
}

const fjreflect::FieldInfo* find_field(const fjreflect::ParseResult& result, const std::string& name) {
    for (const auto& component : result.components) {
        for (const auto& field : component.fields) {
            if (field.name == name) return &field;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("A trailing comment is not part of the field", "[fjreflect]") {
    auto result = parse_header("trailing.hpp", R"(
class hud_controller : public fjell::UiWidget {
  FJELL_UI_WIDGET(hud_controller)

public:
  [[fj::state]] std::string selected;      // hotkey of the selected slot, "1".."8"
  [[fj::state]] std::string label;         /* shown above the hotbar */
  [[fj::state]] int hp = 0;                // with an initializer
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 3);

    const auto* selected = find_field(result, "selected");
    REQUIRE(selected != nullptr);
    REQUIRE(selected->cpp_type == "std::string");
    REQUIRE(selected->is_state);
    REQUIRE(find_field(result, "label") != nullptr);
    REQUIRE(find_field(result, "hp")->cpp_type == "int");
}

TEST_CASE("A commented-out attribute declares nothing", "[fjreflect]") {
    auto result = parse_header("commented.hpp", R"(
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Mover)

public:
  // [[fj::property]] float old_speed = 1.0f;
  /* [[fj::property]] float older_speed = 1.0f; */
  /// Tag a field with [[fj::property]] to show it in the inspector.
  [[fj::property]] float speed = 2.0f;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 1);
    REQUIRE(result.components[0].fields[0].name == "speed");
}

TEST_CASE("A brace in a comment leaves the class intact", "[fjreflect]") {
    auto result = parse_header("brace.hpp", R"(
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Mover)

public:
  // an unmatched { in a comment
  [[fj::property]] float speed = 2.0f;
  /* and one } the other way */
  [[fj::property]] int lives = 3;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(find_field(result, "speed") != nullptr);
    REQUIRE(find_field(result, "lives") != nullptr);
}

TEST_CASE("Comment markers inside literals are text", "[fjreflect]") {
    auto result = parse_header("literals.hpp", R"HDR(
class Link : public fjell::ScriptComponent {
  FJELL_COMPONENT(Link)

public:
  [[fj::property]] std::string url = "http://fjell.dev/*not a comment*/"; // but this is
  [[fj::property]] int budget = 1'000'000; // a digit separator, then a comment
  [[fj::property]] std::string raw = R"(// still text)"; // a comment
};
)HDR");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 3);
    REQUIRE(find_field(result, "url") != nullptr);
    REQUIRE(find_field(result, "budget")->cpp_type == "int");
    REQUIRE(find_field(result, "raw") != nullptr);
}

TEST_CASE("Line numbers still point into the file after a block comment", "[fjreflect]") {
    auto result = parse_header("lines.hpp",
        "class Mover : public fjell::ScriptComponent {\n"   // 1
        "  FJELL_COMPONENT(Mover)\n"                        // 2
        "  /* a block comment\n"                            // 3
        "     over three\n"                                 // 4
        "     lines */\n"                                   // 5
        "public:\n"                                         // 6
        "  [[fj::property]] float speed = 2.0f;\n"          // 7
        "};\n");
    REQUIRE(result.errors.empty());
    REQUIRE(find_field(result, "speed") != nullptr);
    REQUIRE(find_field(result, "speed")->line == 7);
}

namespace {

bool has_error(const fjreflect::ParseResult& result, const std::string& text) {
    for (const auto& error : result.errors) {
        if (error.find(text) != std::string::npos) return true;
    }
    return false;
}

const fjreflect::ComponentInfo* find_component(const fjreflect::ParseResult& result,
                                               const std::string& name) {
    for (const auto& component : result.components) {
        if (component.class_name == name) return &component;
    }
    return nullptr;
}

} // namespace

TEST_CASE("An attribute over several lines marks the field on its last line", "[fjreflect]") {
    auto result = parse_header("multiline.hpp", R"(
class Eye : public fjell::ScriptComponent {
  FJELL_COMPONENT(Eye)
public:
  [[fj::property,
    fj::tooltip("How far it sees"),
    fj::category("Senses")]] float sight = 12.0f;
  [[fj::property]]
  int lives = 3;
};
)");
    REQUIRE(result.errors.empty());
    const auto* sight = find_field(result, "sight");
    REQUIRE(sight != nullptr);
    REQUIRE(sight->tooltip == "How far it sees");
    REQUIRE(sight->category == "Senses");
    REQUIRE(sight->line == 7);
    REQUIRE(find_field(result, "lives")->line == 9);
}

TEST_CASE("Hints are read from the attribute's arguments", "[fjreflect]") {
    auto result = parse_header("hints.hpp", R"(
class Tuned : public fjell::ScriptComponent {
  FJELL_COMPONENT(Tuned)
public:
  [[fj::property, fj::range(-89.0, 89.0f), fj::step(0.5)]] float pitch = 0.0f;
  [[fj::enum(Idle, Walk, "Run Fast")]] int mode = 0;
  [[fj::color]] glm::vec3 tint{1.0f, 0.5f, 0.2f};
  [[fj::readonly, fj::replicated]] bool alive = true;
  [[fj::property, fj::shader_type(water, fog)]] fjell::Ref<fjell::Material> look;
};
)");
    REQUIRE(result.errors.empty());
    const auto* pitch = find_field(result, "pitch");
    REQUIRE(pitch->has_range);
    REQUIRE(pitch->hint_min == -89.0f);
    REQUIRE(pitch->hint_max == 89.0f);
    REQUIRE(pitch->hint_speed == 0.5f);
    const auto* mode = find_field(result, "mode");
    REQUIRE(mode->fj_field_type == "FJELL_FIELD_ENUM");
    REQUIRE(mode->enum_labels == std::vector<std::string>{"Idle", "Walk", "Run Fast"});
    REQUIRE(find_field(result, "tint")->fj_field_type == "FJELL_FIELD_COLOR3");
    REQUIRE(find_field(result, "alive")->is_readonly);
    REQUIRE(find_field(result, "alive")->is_replicated);
    const auto* look = find_field(result, "look");
    REQUIRE(look->ref_target == "Material");
    REQUIRE(look->shader_types == std::vector<std::string>{"water", "fog"});
}

TEST_CASE("The component's name is qualified by every enclosing namespace", "[fjreflect]") {
    auto result = parse_header("namespaces.hpp", R"(
namespace game {
namespace ui::hud {
class Bar : public fjell::UiWidget {
  FJELL_UI_COMPONENT(Bar, "health-bar")
public:
  [[fj::state]] float fill = 1.0f;
};
} // namespace ui::hud
} // namespace game
)");
    REQUIRE(result.errors.empty());
    const auto* bar = find_component(result, "Bar");
    REQUIRE(bar != nullptr);
    REQUIRE(bar->qualified_name == "game::ui::hud::Bar");
    REQUIRE(bar->is_ui_component);
    REQUIRE(bar->widget_tag == "health-bar");
}

TEST_CASE("A nested struct's members are not the component's fields", "[fjreflect]") {
    auto result = parse_header("nested.hpp", R"(
class Hud : public fjell::UiWidget {
  FJELL_UI_WIDGET(Hud)
public:
  struct Slot {
    int count = 0;
    std::string name;
  };
  [[fj::state]] int hp = 0;
  enum class Mode { calm, combat };
  using Count = int;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 1);
    REQUIRE(result.components[0].fields[0].name == "hp");
}

TEST_CASE("A pointer is a pointer however it is spaced", "[fjreflect]") {
    auto result = parse_header("pointers.hpp", R"(
class Hud : public fjell::UiWidget {
  FJELL_UI_WIDGET(Hud)
public:
  [[fj::property]] Node3D *target = nullptr;
  [[nodiscard]] class OrbitCamera *orbit() const;
private:
  fjell::SpriteComponent *sprite_{nullptr};
};
)");
    REQUIRE(result.errors.size() == 1);
    REQUIRE(result.errors[0].find("raw pointer 'Node3D* target'") != std::string::npos);
    REQUIRE(result.warnings.size() == 1);
    REQUIRE(result.warnings[0].find("'fjell::SpriteComponent* sprite_'") != std::string::npos);
}

TEST_CASE("Methods, initialisers and directives do not read as fields", "[fjreflect]") {
    auto result = parse_header("noise.hpp", R"(
#define WIDE_MACRO(x) \
  struct x { int broken = {; }
class Busy : public fjell::ScriptComponent {
  FJELL_COMPONENT(Busy)
public:
  Busy() : speed_(1.0f), target_{} { if (speed_ > 0) { speed_ = 2.0f; } }
  int helper() const { return lives * 2; }
  std::function<void(int, float)> on_hit;
  std::map<int, float> table = {{1, 2.0f}, {3, 4.0f}};
  bool operator==(const Busy& other) const = default;
  static int count;
  [[fj::property]] int lives = 3;
  [[fj::property]] std::string label = "a;b{c}";
private:
  float speed_;
  int target_;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 2);
    REQUIRE(find_field(result, "lives") != nullptr);
    REQUIRE(find_field(result, "label") != nullptr);
}

TEST_CASE("UI actions carry their parameter type and trigger", "[fjreflect]") {
    auto result = parse_header("actions.hpp", R"(
class Hud : public fjell::UiWidget {
  FJELL_UI_WIDGET(Hud)
public:
  [[fj::action]] void close();
  [[fj::action]] void select(int slot) { (void)slot; }
  [[fj::action(pressed)]]
  void boost(float amount);
  [[fj::action("Released")]] void aim_at(const glm::vec2& where);
};
)");
    REQUIRE(result.errors.empty());
    const auto* hud = find_component(result, "Hud");
    REQUIRE(hud->actions.size() == 4);
    REQUIRE(hud->actions[0].param == fjreflect::ActionParamType::none);
    REQUIRE(hud->actions[1].param == fjreflect::ActionParamType::int_param);
    REQUIRE(hud->actions[1].param == fjreflect::ActionParamType::int_param);
    REQUIRE(hud->actions[2].param == fjreflect::ActionParamType::float_param);
    REQUIRE(hud->actions[2].trigger == fjreflect::ActionTriggerMode::pressed);
    REQUIRE(hud->actions[3].param == fjreflect::ActionParamType::vec2_param);
    REQUIRE(hud->actions[3].trigger == fjreflect::ActionTriggerMode::released);
}

TEST_CASE("RPCs keep their parameters, and a component with only RPCs is kept", "[fjreflect]") {
    auto result = parse_header("rpcs.hpp", R"(
class Gun : public fjell::ScriptComponent {
  FJELL_COMPONENT(Gun)
public:
  [[fj::rpc(server)]] void request_fire(const glm::vec3& dir, int weapon);
  [[fj::rpc(clients)]]
  void play_effect(int);
  [[fj::rpc(fjell::RpcTarget::all)]] void ping();
};
)");
    REQUIRE(result.errors.empty());
    const auto* gun = find_component(result, "Gun");
    REQUIRE(gun != nullptr);
    REQUIRE(gun->rpcs.size() == 3);
    REQUIRE(gun->rpcs[0].target == fjreflect::RpcTarget::server);
    REQUIRE(gun->rpcs[0].params.size() == 2);
    REQUIRE(gun->rpcs[0].params[0].cpp_type == "glm::vec3");
    REQUIRE(gun->rpcs[0].params[0].name == "dir");
    REQUIRE(gun->rpcs[1].target == fjreflect::RpcTarget::clients);
    REQUIRE(gun->rpcs[1].params[0].name == "p0");
    REQUIRE(gun->rpcs[2].target == fjreflect::RpcTarget::all);
    REQUIRE(gun->rpcs[2].params.empty());
}

TEST_CASE("What fjreflect cannot reflect is an error with a line", "[fjreflect]") {
    auto result = parse_header("errors.hpp", R"(
class Broken : public fjell::ScriptComponent {
  FJELL_COMPONENT(Broken)
public:
  [[fj::property]] fjell::Node3D* raw = nullptr;
  [[fj::property]] double precise = 0.0;
  [[fj::color]] float not_a_colour = 1.0f;
  [[fj::enum(A, B)]] float not_an_int = 0.0f;
  [[fj::shader_type(fog)]] fjell::Ref<fjell::Texture> not_a_material;
  [[fj::propery]] float typo = 1.0f;
  [[fj::property]] float speed() const;
  [[fj::property]] static float shared;
  [[fj::property]] int grid[4];
  [[fj::property]] int a, b;
  [[fj::range(0, "far")]] float reach = 1.0f;
  float [[fj::property]] late = 1.0f;
  [[fj::action]] void jump();
  [[fj::rpc(server)]] void fire() {}
  [[fj::property]] float fine = 1.0f;
};
)");
    REQUIRE(has_error(result, "errors.hpp:5: error: [[fj::property]] on raw pointer"));
    REQUIRE(has_error(result, "errors.hpp:6: error: unsupported type 'double'"));
    REQUIRE(has_error(result, "errors.hpp:7: error: [[fj::color]] requires vec3 or vec4"));
    REQUIRE(has_error(result, "errors.hpp:8: error: [[fj::enum]] requires int field"));
    REQUIRE(has_error(result, "errors.hpp:9: error: [[fj::shader_type]] requires Ref<Material>"));
    REQUIRE(has_error(result, "errors.hpp:10: error: unknown attribute [[fj::propery]]"));
    REQUIRE(has_error(result, "errors.hpp:11: error: fj field attributes mark a data member"));
    REQUIRE(has_error(result, "errors.hpp:12: error: 'shared' is static"));
    REQUIRE(has_error(result, "errors.hpp:13: error: 'grid' is an array"));
    REQUIRE(has_error(result, "errors.hpp:14: error: declare one reflected field per declaration"));
    REQUIRE(has_error(result, "errors.hpp:15: error: [[fj::range]] on 'reach' expects (min, max)"));
    REQUIRE(has_error(result, "errors.hpp:16: error: [[fj::property]] goes before the declaration"));
    REQUIRE(has_error(result, "errors.hpp:17: error: [[fj::action]] on 'jump' belongs in a UI widget"));
    REQUIRE(has_error(result, "errors.hpp:18: error: [[fj::rpc]] method 'fire' is defined by fjreflect"));
    REQUIRE(find_field(result, "fine") != nullptr);
}

TEST_CASE("An fj attribute outside a component is an error", "[fjreflect]") {
    auto result = parse_header("stray.hpp", R"(
struct Settings {
  [[fj::property]] int width = 1920;
};
)");
    REQUIRE(has_error(result, "stray.hpp:3: error: [[fj::property]] on 'width' in class Settings, which is not a component"));
}

TEST_CASE("The component macro names the class it sits in", "[fjreflect]") {
    auto result = parse_header("mismatch.hpp", R"(
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Walker)
public:
  [[fj::property]] float speed = 1.0f;
};
)");
    REQUIRE(has_error(result, "mismatch.hpp:3: error: FJELL_COMPONENT(Walker) sits in class Mover"));
}

TEST_CASE("A class that never closes is an error at its opening line", "[fjreflect]") {
    auto result = parse_header("unclosed.hpp", R"(
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Mover)
public:
  [[fj::property]] float speed = 1.0f;
)");
    REQUIRE(has_error(result, "unclosed.hpp:2: error: unexpected end of file: class Mover is never closed"));
}

TEST_CASE("[[fj::bind]] is refused, naming what replaces it", "[fjreflect]") {
    auto result = parse_header("bound.hpp", R"(
class Hud : public fjell::UiWidget {
  FJELL_UI_WIDGET(Hud)
public:
  [[fj::bind]] int hp = 0;
};
class Player : public fjell::ScriptComponent {
  FJELL_COMPONENT(Player)
public:
  [[fj::bind]] Hud* hud = nullptr;
};
)");
    REQUIRE(result.errors.size() == 2);
    REQUIRE(result.errors[0].find("bound.hpp:5: error: [[fj::bind]] on 'hp' is gone: mark a field the markup "
                                  "reads [[fj::state]]") != std::string::npos);
    REQUIRE(result.errors[1].find("bound.hpp:10: error: [[fj::bind]] on 'hud' is gone") != std::string::npos);
}

TEST_CASE("A script component says how it ticks on its class head", "[fjreflect]") {
    auto result = parse_header("tick.hpp", R"(
class [[fj::tick(order = 10)]] Follower : public fjell::ScriptComponent {
  FJELL_COMPONENT(Follower)
  [[fj::property]] float lag = 0.2f;
};
// Nothing else to reflect: kept for the order alone.
class [[fj::tick(order = -5)]] Leader : public fjell::ScriptComponent {
  FJELL_COMPONENT(Leader)
};
class Plain : public fjell::ScriptComponent {
  FJELL_COMPONENT(Plain)
  [[fj::property]] int hp = 1;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(find_component(result, "Follower") != nullptr);
    CHECK(find_component(result, "Follower")->tick_order == 10);
    REQUIRE(find_component(result, "Leader") != nullptr);
    CHECK(find_component(result, "Leader")->tick_order == -5);
    CHECK(find_component(result, "Plain")->tick_order == 0);
    CHECK_FALSE(find_component(result, "Plain")->has_tick);
}

TEST_CASE("fj::tick is reported where it cannot be read or does not belong", "[fjreflect]") {
    CHECK(has_error(parse_header("tick_key.hpp", R"(
class [[fj::tick(speed = 2)]] A : public fjell::ScriptComponent { FJELL_COMPONENT(A) };
)"), "has no key 'speed'"));
    CHECK(has_error(parse_header("tick_whole.hpp", R"(
class [[fj::tick(order = 1.5)]] B : public fjell::ScriptComponent { FJELL_COMPONENT(B) };
)"), "order is a whole number"));
    CHECK(has_error(parse_header("tick_bare.hpp", R"(
class [[fj::tick]] C : public fjell::ScriptComponent { FJELL_COMPONENT(C) };
)"), "takes keys"));
    CHECK(has_error(parse_header("tick_widget.hpp", R"(
class [[fj::tick(order = 1)]] D : public fjell::UiWidget { FJELL_UI_WIDGET(D) };
)"), "only a script component"));
    CHECK(has_error(parse_header("tick_plain.hpp", R"(
struct [[fj::tick(order = 1)]] E { int x; };
)"), "not a component"));
    CHECK(has_error(parse_header("tick_other.hpp", R"(
class [[fj::property]] F : public fjell::ScriptComponent { FJELL_COMPONENT(F) };
)"), "does not apply to a class"));
}
