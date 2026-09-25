#include "fjreflect/generator.hpp"
#include "fjreflect/parser.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Several headers read as one project: parsed one by one, then linked, the
// way fjreflect's main does it.
struct Project {
    std::vector<fjreflect::ComponentInfo> components;
    std::vector<fjreflect::ClassDecl> classes;
    std::vector<fjreflect::PipeInfo> pipes;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    fjreflect::LinkResult linked;

    [[nodiscard]] bool has_error(const std::string& text) const {
        for (const auto& e : errors) {
            if (e.find(text) != std::string::npos) return true;
        }
        return false;
    }
    [[nodiscard]] const fjreflect::StructInfo* find_struct(const std::string& qualified) const {
        for (const auto& s : linked.structs) {
            if (s.qualified_name == qualified) return &s;
        }
        return nullptr;
    }
    [[nodiscard]] const fjreflect::FieldInfo* find_field(const std::string& owner, const std::string& name) const {
        for (const auto& c : components) {
            if (c.class_name != owner) continue;
            for (const auto& f : c.fields) {
                if (f.name == name) return &f;
            }
        }
        for (const auto& s : linked.structs) {
            if (s.name != owner) continue;
            for (const auto& f : s.fields) {
                if (f.name == name) return &f;
            }
        }
        return nullptr;
    }
};

Project read_project(const std::vector<std::pair<std::string, std::string>>& headers) {
    const auto dir = fs::temp_directory_path() / ("fjreflect_models_" + std::to_string(fjell::platform::process_id()));
    fs::create_directories(dir);
    Project project;
    for (const auto& [name, text] : headers) {
        const auto path = dir / name;
        std::ofstream(path) << text;
        auto result = fjreflect::parse_file(path.string());
        for (auto& c : result.components) project.components.push_back(std::move(c));
        for (auto& c : result.classes) project.classes.push_back(std::move(c));
        for (auto& p : result.pipes) project.pipes.push_back(std::move(p));
        for (auto& e : result.errors) project.errors.push_back(std::move(e));
        for (auto& w : result.warnings) project.warnings.push_back(std::move(w));
        fs::remove(path);
    }
    if (project.errors.empty()) {
        project.linked = fjreflect::link(project.components, project.classes, project.pipes);
        for (const auto& e : project.linked.errors) project.errors.push_back(e);
        for (const auto& w : project.linked.warnings) project.warnings.push_back(w);
    }
    return project;
}

constexpr const char* HUD_MODEL = R"(
#pragma once
#include <string>
#include <vector>

namespace game {

struct SlotModel {
  int index = 0;
  std::string name;
  int count = 0;
  [[fj::state]] bool empty() const { return name.empty(); }
};

struct HudModel {
  std::vector<SlotModel> slots;
  SlotModel selected;
  std::vector<int> recent;
  bool calm = false;
};

} // namespace game
)";

constexpr const char* HUD_CONTROLLER = R"(
#pragma once
#include "hud_model.hpp"

namespace game {

class hud_controller : public fjell::UiWidget {
  FJELL_UI_WIDGET(hud_controller)
public:
  [[fj::model]] HudModel hud;
  [[fj::action]] void select_slot(int index);
};

class PlayerStats : public fjell::ScriptComponent {
  FJELL_COMPONENT(PlayerStats)
public:
  [[fj::state]] bool low_health() const { return hp * 4 < max_hp; }
  [[fj::state]] const std::string& title() const { return title_; }
private:
  [[fj::property]] int max_hp = 100;
  [[fj::state]] int hp = 100;
  std::string title_;
};

} // namespace game
)";

} // namespace

TEST_CASE("A Model's struct is found in another header and reflected in full", "[fjreflect]") {
    const auto project = read_project({{"hud_model.hpp", HUD_MODEL}, {"hud_controller.hpp", HUD_CONTROLLER}});
    REQUIRE(project.errors.empty());

    const auto* hud = project.find_field("hud_controller", "hud");
    REQUIRE(hud != nullptr);
    REQUIRE(hud->is_model);
    REQUIRE(hud->is_state);
    REQUIRE(hud->fj_field_type == "FJELL_FIELD_STRUCT");
    REQUIRE(hud->struct_type == "game::HudModel");

    const auto* model = project.find_struct("game::HudModel");
    REQUIRE(model != nullptr);
    REQUIRE(model->fields.size() == 4);
    const auto* slots = project.find_field("HudModel", "slots");
    REQUIRE(slots->fj_field_type == "FJELL_FIELD_VECTOR");
    REQUIRE(slots->element_fj_type == "FJELL_FIELD_STRUCT");
    REQUIRE(slots->element_struct == "game::SlotModel");
    REQUIRE(project.find_field("HudModel", "recent")->element_fj_type == "FJELL_FIELD_INT");
    REQUIRE(project.find_field("HudModel", "selected")->struct_type == "game::SlotModel");

    // Every member of a reflected struct is state, and its [[fj::state]]
    // getter comes with it.
    REQUIRE(project.find_struct("game::SlotModel") != nullptr);
    const auto* empty = project.find_field("SlotModel", "empty");
    REQUIRE(empty->is_getter);
    REQUIRE(empty->fj_field_type == "FJELL_FIELD_BOOL");
    REQUIRE(project.find_field("SlotModel", "count")->is_state);
}

TEST_CASE("A component's state and getters are reflected, private ones too", "[fjreflect]") {
    const auto project = read_project({{"hud_model.hpp", HUD_MODEL}, {"hud_controller.hpp", HUD_CONTROLLER}});
    REQUIRE(project.errors.empty());

    const auto* hp = project.find_field("PlayerStats", "hp");
    REQUIRE(hp->is_state);
    REQUIRE_FALSE(hp->is_getter);
    REQUIRE_FALSE(project.find_field("PlayerStats", "max_hp")->is_state);
    const auto* low = project.find_field("PlayerStats", "low_health");
    REQUIRE(low->is_getter);
    REQUIRE(low->fj_field_type == "FJELL_FIELD_BOOL");
    const auto* title = project.find_field("PlayerStats", "title");
    REQUIRE(title->cpp_type == "std::string"); // returned by reference, read as the value
}

TEST_CASE("A struct nested in a class is found from inside it", "[fjreflect]") {
    const auto project = read_project({{"nested.hpp", R"(
class Inventory : public fjell::UiWidget {
  FJELL_UI_WIDGET(Inventory)
public:
  struct Item {
    std::string name;
    std::vector<Item> contents;
  };
  struct Model {
    std::vector<Item> items;
  };
  [[fj::model]] Model model;
};
)"}});
    REQUIRE(project.errors.empty());
    REQUIRE(project.find_field("Inventory", "model")->struct_type == "Inventory::Model");
    // A struct may hold a list of itself.
    REQUIRE(project.find_field("Item", "contents")->element_struct == "Inventory::Item");
    REQUIRE(project.linked.structs.size() == 2);
}

TEST_CASE("Pipes are free functions formatting a value", "[fjreflect]") {
    const auto project = read_project({{"pipes.hpp", R"(
#include <string>
namespace game {
[[fj::pipe]] inline std::string coins(int copper, const std::string& unit) { return std::to_string(copper) + unit; }
[[fj::pipe]] std::string percent(float ratio);
}
)"}});
    REQUIRE(project.errors.empty());
    REQUIRE(project.pipes.size() == 2);
    REQUIRE(project.pipes[0].name == "coins");
    REQUIRE(project.pipes[0].qualified_name == "game::coins");
    REQUIRE(project.pipes[0].input_fj_type == "FJELL_FIELD_INT");
    REQUIRE(project.pipes[0].arg_cpp_types == std::vector<std::string>{"std::string"});
    REQUIRE(project.pipes[1].input_fj_type == "FJELL_FIELD_FLOAT");
}

TEST_CASE("What a Model cannot hold is an error with a line", "[fjreflect]") {
    const auto parsed = read_project({{"bad_model.hpp", R"(
class Panel : public fjell::UiWidget {
  FJELL_UI_WIDGET(Panel)
public:
  [[fj::state]] int getter_without_const();
  [[fj::state]] int getter_with_params(int x) const;
  [[fj::state]] std::vector<bool> flags;
  [[fj::model]] int not_a_struct;
  [[fj::property, fj::state]] int both;
  [[fj::state, fj::replicated]] std::vector<int> synced;
  [[fj::state]] void* raw;
};
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Mover)
public:
  [[fj::model]] int model;
};
[[fj::pipe]] int not_text(int x);
[[fj::pipe]] std::string no_value();
[[fj::pipe]] std::string bad_arg(int x, bool b);
)"}});
    REQUIRE(parsed.has_error("bad_model.hpp:5: error: getter 'getter_without_const' must be const"));
    REQUIRE(parsed.has_error("bad_model.hpp:6: error: getter 'getter_with_params' must take no parameters"));
    REQUIRE(parsed.has_error("bad_model.hpp:7: error: 'flags': std::vector<bool>"));
    REQUIRE(parsed.has_error("bad_model.hpp:8: error: [[fj::model]] on 'not_a_struct' needs a struct"));
    REQUIRE(parsed.has_error("bad_model.hpp:9: error: 'both' is either authored"));
    REQUIRE(parsed.has_error("bad_model.hpp:10: error: 'synced' holds a struct or a list, which cannot be replicated"));
    REQUIRE(parsed.has_error("bad_model.hpp:11: error: 'raw': fjreflect has no field kind for 'void*'"));
    REQUIRE(parsed.has_error("bad_model.hpp:16: error: [[fj::model]] on 'model' belongs in a UI widget"));
    REQUIRE(parsed.has_error("bad_model.hpp:18: error: pipe 'not_text' must return std::string"));
    REQUIRE(parsed.has_error("bad_model.hpp:19: error: pipe 'no_value' needs a first parameter"));
    REQUIRE(parsed.has_error("bad_model.hpp:20: error: pipe 'bad_arg': argument 1 is a literal in markup"));

    const auto linked = read_project({{"bad_link.hpp", R"(
struct Secret {
  int shown = 0;
private:
  int hidden = 0;
};
struct Holder {
  Secret secret;
  Missing missing;
  fjell::Ref<fjell::Texture> icon;
};
class Screen : public fjell::UiWidget {
  FJELL_UI_WIDGET(Screen)
public:
  [[fj::model]] Holder holder;
  [[fj::model]] Screen itself;
};
[[fj::pipe]] std::string twice(int x);
[[fj::pipe]] std::string twice(float x);
)"}});
    REQUIRE(linked.has_error("bad_link.hpp:5: error: 'Screen::holder → Holder::secret → Secret::hidden' is private"));
    REQUIRE(linked.has_error("bad_link.hpp:9: error: 'Screen::holder → Holder::missing' holds 'Missing', which no "
                             "script header declares"));
    REQUIRE(linked.has_error("bad_link.hpp:10: error: 'Screen::holder → Holder::icon' is a reference"));
    REQUIRE(linked.has_error("bad_link.hpp:16: error: 'Screen::itself' holds Screen, a component"));
    REQUIRE(linked.has_error("bad_link.hpp:19: error: a pipe named 'twice' is already declared"));
}

TEST_CASE("A [[fj::state]] no Model reaches is worth a warning", "[fjreflect]") {
    const auto project = read_project({{"lonely.hpp", R"(
struct Lonely {
  [[fj::state]] bool ready() const { return true; }
};
)"}});
    REQUIRE(project.errors.empty());
    REQUIRE(project.warnings.size() == 1);
    REQUIRE(project.warnings[0].find("lonely.hpp:3: warning: [[fj::state]] on 'ready' in Lonely") !=
            std::string::npos);
}

TEST_CASE("Generated tables describe Models, getters, lists and pipes", "[fjreflect]") {
    auto project = read_project({{"hud_model.hpp", HUD_MODEL},
                                 {"hud_controller.hpp", HUD_CONTROLLER},
                                 {"pipes.hpp", R"(
#include <string>
[[fj::pipe]] std::string coins(int copper, const std::string& unit);
)"}});
    REQUIRE(project.errors.empty());
    const auto files = fjreflect::generate(project.components, project.linked.structs, project.pipes);

    const auto& model = files.at("hud_model.generated.cpp");
    // A header of plain structs includes nothing from the SDK, so its file
    // declares the template its tables specialise.
    REQUIRE(model.find("namespace fjell { template <class T> struct FjellAccess; }") != std::string::npos);
    // The list is read through functions generated for its exact type.
    REQUIRE(model.find("static uint32_t length_slots(const void* v) { return static_cast<uint32_t>("
                       "static_cast<const decltype(game::HudModel::slots)*>(v)->size()); }") != std::string::npos);
    REQUIRE(model.find("static const FjellVectorDesc fjreflect_vector_game__HudModel_slots = {FJELL_FIELD_STRUCT, "
                       "&fjreflect_type_game__SlotModel,") != std::string::npos);
    REQUIRE(model.find("static void get_empty(const void* self, void* out) { *static_cast<bool*>(out) = "
                       "static_cast<const game::SlotModel*>(self)->empty(); }") != std::string::npos);
    REQUIRE(model.find("extern \"C\" const FjellTypeDesc fjreflect_type_game__HudModel = {\"HudModel\", "
                       "\"game::HudModel\", sizeof(game::HudModel), alignof(game::HudModel), "
                       "fjreflect_fields_game__HudModel, 4,") != std::string::npos);

    const auto& controller = files.at("hud_controller.generated.cpp");
    REQUIRE(controller.find("extern \"C\" const FjellTypeDesc fjreflect_type_game__HudModel;") != std::string::npos);
    REQUIRE(controller.find("FJELL_FIELD_FLAG_STATE | FJELL_FIELD_FLAG_MODEL, &fjreflect_type_game__HudModel") !=
            std::string::npos);
    REQUIRE(controller.find("FJELL_FIELD_FLAG_STATE | FJELL_FIELD_FLAG_GETTER, nullptr, nullptr, "
                            "fjell::FjellAccess<game::PlayerStats>::get_low_health}") != std::string::npos);

    REQUIRE(files.at("pipes.generated.cpp").find("*static_cast<std::string*>(out) = coins(*static_cast<const int*>"
                                                 "(input), *static_cast<const std::string*>(args[0]));") !=
            std::string::npos);

    const auto reg = fjreflect::register_file(project.components, project.linked.structs, project.pipes);
    REQUIRE(reg.find("fjell_reg_types(registry, fjreflect_types, 2);") != std::string::npos);
    REQUIRE(reg.find("{\"coins\", FJELL_FIELD_INT, fjreflect_pipe_coins_args, 1, fjreflect_pipe_coins},") !=
            std::string::npos);
    // Structs and pipes go to the engine before the components that hold them.
    REQUIRE(reg.find("fjell_reg_types") < reg.find("fjell_auto_reg_head"));

    const auto manifest = nlohmann::json::parse(files.at("manifest.json"));
    REQUIRE(manifest["structs"].size() == 2);
    REQUIRE(manifest["pipes"][0]["args"][0] == "string");
}
