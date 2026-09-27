#include "ui/game_ui/widget_check.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

// A game with a stats component, a HUD controller holding a Model of slots
// and effects, and one pipe of its own, as fjreflect describes it.
nlohmann::json game_manifest() {
    return nlohmann::json::parse(R"({
        "version": 2,
        "types": [
            {"name": "Stats", "kind": "component", "fields": [
                {"name": "hp", "kind": "int"},
                {"name": "max_hp", "kind": "int"},
                {"name": "low", "kind": "bool", "getter": true},
                {"name": "pos", "kind": "vec2"},
                {"name": "target", "kind": "weak_ref"}
            ]},
            {"name": "hud", "kind": "ui_widget", "fields": [
                {"name": "model", "kind": "struct", "struct": "HudModel"},
                {"name": "title", "kind": "string"}
            ], "actions": [
                {"name": "select", "param": "int"},
                {"name": "close", "param": "none"},
                {"name": "aim", "param": "vec2"}
            ]}
        ],
        "structs": [
            {"name": "Slot", "fields": [
                {"name": "key", "kind": "int"},
                {"name": "name", "kind": "string"}
            ]},
            {"name": "Effect", "fields": [{"name": "left", "kind": "float"}]},
            {"name": "HudModel", "fields": [
                {"name": "slots", "kind": "list", "element": "struct", "element_struct": "Slot"},
                {"name": "names", "kind": "list", "element": "string"},
                {"name": "selected", "kind": "int"}
            ]}
        ],
        "pipes": [{"name": "stars", "input": "int", "args": ["int"]}]
    })");
}

UiSchema game() {
    return UiSchema::from_json(game_manifest());
}

struct Project {
    fs::path base;

    Project() {
        base = fs::temp_directory_path() / ("fjell_widget_check_" + std::to_string(platform::process_id()));
        fs::create_directories(base / "ui" / "widgets");
        write("ui/widgets/slot_face.fjwidget",
              "<widget>\n"
              "    <prop name=\"slot\" type=\"Slot\"/>\n"
              "    <prop name=\"size\" type=\"int\"/>\n"
              "    <div on:click=\"activate\"><text>{slot.name}</text></div>\n"
              "</widget>\n");
    }
    ~Project() { fs::remove_all(base); }

    std::string write(const std::string& rel, const std::string& text) const {
        std::ofstream(base / rel) << text;
        return (base / rel).string();
    }

    // A HUD file around the given markup: the hud controller, a stats Model
    // and a title prop.
    std::string hud(const std::string& markup) const {
        return write("ui/hud.fjwidget", "<widget class=\"hud\">\n"
                                        "    <model name=\"player\" component=\"Stats\"/>\n"
                                        "    <prop name=\"caption\" type=\"string\"/>\n" +
                                            markup + "</widget>\n");
    }
};

std::vector<WidgetProblem> check(const std::string& path) {
    return check_widget(path, game()).problems;
}

std::string messages(const std::vector<WidgetProblem>& problems) {
    std::string out;
    for (const auto& p : problems) out += std::to_string(p.line) + ":" + std::to_string(p.column) + " " + p.message + "\n";
    return out;
}

} // namespace

TEST_CASE("A widget reading only what its game declares has no problems", "[widget-check]") {
    Project project;
    const auto path = project.hud(
        "    <div>\n"
        "        <text>{caption}: {player.hp|stars:5} / {player.max_hp|round} {title}</text>\n"
        "        <bar value=\"{player.pos.x}\" if=\"player.low\" class:hot=\"model.selected == 2\"/>\n"
        "        <slot-face foreach=\"s, i in model.slots\" slot=\"{s}\" size=\"{i}\" on:activate=\"select({s.key})\"/>\n"
        "        <text foreach=\"n in model.names\">{n}</text>\n"
        "        <div on:click=\"close\"/><div on:click=\"aim(0.5, 1)\"/>\n"
        "    </div>\n");
    const auto problems = check(path);
    INFO(messages(problems));
    REQUIRE(problems.empty());
}

TEST_CASE("A field the type does not have is reported where it is written", "[widget-check]") {
    Project project;
    const auto path = project.hud("    <div>\n"
                                  "        <text>{player.health}</text>\n"
                                  "    </div>\n");
    const auto problems = check(path);
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].file == path);
    REQUIRE(problems[0].line == 5);
    REQUIRE(problems[0].column == 16);
    REQUIRE(problems[0].message == "'Stats' has no field 'health'");
}

TEST_CASE("A first name nothing declares is reported", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <text>{caption} {subtitle}</text>\n"));
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].line == 4);
    REQUIRE(problems[0].column == 22);
    REQUIRE(problems[0].message.find("nothing here is named 'subtitle'") != std::string::npos);
}

TEST_CASE("A path is followed through structs, lists and vectors", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <div>\n"
                                            "        <text>{model.slots.name}</text>\n"
                                            "        <text>{player.pos.z}</text>\n"
                                            "        <text>{player.hp.value}</text>\n"
                                            "        <text>{player.target.name}</text>\n"
                                            "    </div>\n"));
    REQUIRE(problems.size() == 3);
    REQUIRE(problems[0].message == "'model.slots' is a list: repeat it with foreach to read 'name'");
    REQUIRE(problems[1].message == "'player.pos' is a vec2, which has no component 'z'");
    REQUIRE(problems[2].message == "'player.hp' is an int, which has no field 'value'");
    // A reference is not a value markup reads: nothing past it is judged.
}

TEST_CASE("A foreach names its entry and index for what is inside it", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <div>\n"
                                            "        <div foreach=\"s, i in model.slots\"><text>{i} {s.name} {s.icon}</text></div>\n"
                                            "        <text>{s.name}</text>\n"
                                            "        <div foreach=\"x in model.selected\"/>\n"
                                            "        <div foreach=\"model.slots\"/>\n"
                                            "    </div>\n"));
    INFO(messages(problems));
    REQUIRE(problems.size() == 4);
    REQUIRE(problems[0].message == "'Slot' has no field 'icon'");
    REQUIRE(problems[1].message.find("nothing here is named 's'") != std::string::npos);
    REQUIRE(problems[2].message == "'model.selected' is not a list, so foreach cannot repeat over it");
    REQUIRE(problems[3].message == "foreach reads `item in list` or `item, i in list`");
}

TEST_CASE("Pipes the engine and the game lack are reported", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <text>{player.hp|sparkle} {player.hp|stars} {player.hp|fixed:1}</text>\n"));
    REQUIRE(problems.size() == 2);
    REQUIRE(problems[0].message == "no pipe named 'sparkle' in the engine or the game");
    REQUIRE(problems[1].message == "|stars takes 1 argument(s), given 0");
}

TEST_CASE("Props, Models and controllers name things the game has", "[widget-check]") {
    Project project;
    const auto path = project.write("ui/other.fjwidget",
                                    "<widget class=\"menu\">\n"
                                    "    <model name=\"unit\" component=\"Slot\"/>\n"
                                    "    <prop name=\"thing\" type=\"Thing\"/>\n"
                                    "    <prop name=\"slot\" type=\"Slot\"/>\n"
                                    "    <text>{unit.key} {thing.x} {slot.key}</text>\n"
                                    "</widget>\n");
    const auto problems = check(path);
    INFO(messages(problems));
    REQUIRE(problems.size() == 3);
    REQUIRE(problems[0].line == 1);
    REQUIRE(problems[0].message == "controller 'menu' is not a UI widget the game has");
    REQUIRE(problems[1].line == 2);
    REQUIRE(problems[1].message == "model 'unit' reads 'Slot', which is not a script component the game has");
    REQUIRE(problems[2].line == 3);
    REQUIRE(problems[2].message.find("prop 'thing' has type 'Thing'") != std::string::npos);
}

TEST_CASE("Actions are checked against the controller that answers them", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <div>\n"
                                            "        <div on:click=\"select(two)\"/>\n"
                                            "        <div on:click=\"selcet(2)\"/>\n"
                                            "        <div on:click=\"close(1)\"/>\n"
                                            "        <div on:click=\"select({nope})\"/>\n"
                                            "    </div>\n"));
    INFO(messages(problems));
    REQUIRE(problems.size() == 4);
    REQUIRE(problems[0].message == "select takes a whole number; 'two' is not a number");
    REQUIRE(problems[1].message == "no controller around it has an [[fj::action]] named 'selcet'");
    REQUIRE(problems[2].message == "close takes nothing, given 1 argument");
    REQUIRE(problems[3].message.find("nothing here is named 'nope'") != std::string::npos);
}

TEST_CASE("A placement gives the props and maps the events the widget has", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <div>\n"
                                            "        <slot-face slot=\"{player}\" colour=\"red\" size=\"{player.pos}\" on:activate=\"close\"/>\n"
                                            "        <slot-face slot=\"{model.slots}\" on:pick=\"close\" on:activate=\"close\"/>\n"
                                            "        <slot-face/>\n"
                                            "    </div>\n"));
    INFO(messages(problems));
    REQUIRE(problems.size() == 6);
    REQUIRE(problems[0].message == "prop 'slot' takes Slot, given Stats");
    REQUIRE(problems[1].message == "<slot-face> has no prop 'colour'");
    REQUIRE(problems[2].message == "prop 'size' takes a number, given vec2");
    REQUIRE(problems[3].message == "prop 'slot' takes Slot, given list");
    REQUIRE(problems[4].message == "<slot-face> raises no event 'pick'");
    REQUIRE(problems[5].message ==
            "<slot-face> raises 'activate', which no controller around it answers: map it with on:activate");
}

TEST_CASE("A tag that is neither an element nor a widget is reported", "[widget-check]") {
    Project project;
    const auto problems = check(project.hud("    <slto-face/>\n"));
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].column == 5);
    REQUIRE(problems[0].message == "<slto-face> is neither an element the engine draws nor a widget in widgets/slto_face.fjwidget");
}

TEST_CASE("A placed widget's clicks are events for whoever places it", "[widget-check]") {
    Project project;
    // slot_face raises `activate` and answers nothing itself.
    const auto problems = check((project.base / "ui/widgets/slot_face.fjwidget").string());
    INFO(messages(problems));
    REQUIRE(problems.empty());
}

TEST_CASE("Without a manifest only the widget's own names are judged", "[widget-check]") {
    Project project;
    const auto path = project.write("ui/widgets/undeclared.fjwidget",
                                    "<widget>\n"
                                    "    <prop name=\"title\" type=\"string\"/>\n"
                                    "    <div><text>{title} {subtitle}</text></div>\n"
                                    "</widget>\n");
    const auto problems = check_widget(path, UiSchema{}).problems;
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].line == 3);
    REQUIRE(problems[0].message.find("nothing here is named 'subtitle'") != std::string::npos);

    // A controller the checker cannot see may have any field and action.
    const auto hud = project.hud("    <text on:click=\"whatever\">{anything.at.all|sparkle}</text>\n");
    REQUIRE(check_widget(hud, UiSchema{}).problems.empty());
}

TEST_CASE("The manifest is found above the widget", "[widget-check]") {
    Project project;
    fs::create_directories(project.base / "scripts" / "generated");
    project.write("scripts/generated/manifest.json", R"({"version": 2, "types": [], "pipes": []})");
    const auto found = UiSchema::find_manifest((project.base / "ui/widgets/slot_face.fjwidget").string());
    REQUIRE(fs::path(found) == project.base / "scripts" / "generated" / "manifest.json");
    auto schema = UiSchema::load(found);
    REQUIRE(schema);
    REQUIRE(schema->reflected);
}

TEST_CASE("A check names every type and game pipe its answer depends on", "[widget-check]") {
    Project project;
    const auto path = project.hud("    <div>\n"
                                  "        <text>{player.hp|stars:5}</text>\n"
                                  "        <slot-face foreach=\"s in model.slots\" slot=\"{s}\" on:activate=\"close\"/>\n"
                                  "    </div>\n");
    const auto result = check_widget(path, game());
    INFO(messages(result.problems));
    REQUIRE(result.problems.empty());
    REQUIRE(result.types == std::vector<std::string>{"HudModel", "Slot", "Stats", "hud"});
    REQUIRE(result.pipes == std::vector<std::string>{"stars"});
    REQUIRE(result.placed.size() == 1);
    REQUIRE(fs::path(result.placed[0]) == project.base / "ui/widgets/slot_face.fjwidget");
}

TEST_CASE("A reload reports the widgets a changed declaration touches", "[widget-check]") {
    Project project;
    fs::create_directories(project.base / "scripts" / "generated");
    const std::string manifest = (project.base / "scripts/generated/manifest.json").string();
    auto build = [&](const nlohmann::json& declared) { std::ofstream(manifest) << declared.dump(); };

    const std::vector<std::string> widgets{
        project.hud("    <text>{player.hp}</text>\n"),
        project.write("ui/widgets/slot_name.fjwidget", "<widget>\n"
                                                       "    <prop name=\"slot\" type=\"Slot\"/>\n"
                                                       "    <text>{slot.name} {slot.nope}</text>\n"
                                                       "</widget>\n"),
    };
    WidgetChecks checks;

    // The first load reports every widget.
    build(game_manifest());
    auto problems = checks.reload(manifest, widgets);
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].message == "'Slot' has no field 'nope'");
    const uint32_t first = checks.revision();

    // Nothing declared differently: nothing to say, nothing to check again.
    REQUIRE(checks.reload(manifest, widgets).empty());
    REQUIRE(checks.revision() == first);

    // Stats loses hp: the HUD reads it, the slot widget never names Stats.
    auto renamed = game_manifest();
    renamed["types"][0]["fields"][0]["name"] = "health";
    build(renamed);
    problems = checks.reload(manifest, widgets);
    REQUIRE(problems.size() == 1);
    REQUIRE(problems[0].file == widgets[0]);
    REQUIRE(problems[0].message == "'Stats' has no field 'hp'");
    REQUIRE(checks.revision() != first);
    REQUIRE(checks.check(widgets[0]).problems.size() == 1);
}

TEST_CASE("A controller's [[fj::prop]] needs the widget's prop of its name and kind", "[widget-check]") {
    Project project;
    auto manifest = game_manifest();
    manifest["types"].push_back(nlohmann::json::parse(R"({"name": "slot_controller", "kind": "ui_widget", "fields": [
        {"name": "index", "kind": "int", "prop": true},
        {"name": "label", "kind": "string", "prop": true},
        {"name": "hot", "kind": "bool", "prop": true}
    ]})"));
    const auto path = project.write("ui/widgets/slot.fjwidget", "<widget class=\"slot_controller\">\n"
                                                              "    <prop name=\"index\" type=\"int\"/>\n"
                                                              "    <prop name=\"label\" type=\"float\"/>\n"
                                                              "    <text>{index}</text>\n"
                                                              "</widget>\n");
    const auto problems = check_widget(path, UiSchema::from_json(manifest)).problems;
    INFO(messages(problems));
    REQUIRE(problems.size() == 2);
    REQUIRE(problems[0].line == 1);
    REQUIRE(problems[0].message == "the controller's [[fj::prop]] 'hot' has no <prop name=\"hot\"> to be filled from");
    REQUIRE(problems[1].line == 3);
    REQUIRE(problems[1].message == "<prop> 'label' is float, but the controller's [[fj::prop]] of that name is string");
}
