#include "platform/platform.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/component.hpp"
#include "scripting/reflected_types.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_models.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/widget_asset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>

using namespace fjell;

namespace {

// A script component as a game declares one, and the field table fjreflect
// would generate for it.
struct Stats : ScriptComponent {
    float hp{0.0f};
};

OwnedFieldDesc hp_field() {
    const Stats probe;
    OwnedFieldDesc field{};
    field.name = "hp";
    field.type = FJELL_FIELD_FLOAT;
    field.offset = static_cast<uint32_t>(reinterpret_cast<const char*>(&probe.hp) -
                                         reinterpret_cast<const char*>(static_cast<const ScriptComponent*>(&probe)));
    field.size = sizeof(float);
    field.flags = FJELL_FIELD_FLAG_STATE;
    return field;
}

/// The game as the Models see it: one component type, and a set of nodes
/// another player controls.
struct TestWorld : UiModelWorld {
    std::string stats_name{"Stats"};
    std::vector<OwnedFieldDesc> stats_fields{hp_field()};
    std::unordered_set<uint32_t> others;
    uint64_t possession{0};

    [[nodiscard]] std::optional<UiComponentType> component_type(const std::string& name) const override {
        if (name != stats_name) return std::nullopt;
        return UiComponentType{stats_name, stats_fields};
    }
    [[nodiscard]] bool owned_by_local(uint32_t node_id) const override { return !others.contains(node_id); }
    [[nodiscard]] uint64_t possession_revision() const override { return possession; }

    /// Hands a node to another player, or back.
    void give_away(uint32_t node, bool away) {
        if (away) others.insert(node);
        else others.erase(node);
        ++possession;
    }
};

/// A node carrying a Stats script at `hp`.
uint32_t spawn(Scene& scene, const char* name, float hp) {
    auto node = std::make_unique<Node3D>();
    node->name = name;
    auto stats = std::make_unique<Stats>();
    stats->hp = hp;
    ScriptEntry entry;
    entry.type_name = "Stats";
    entry.instance = std::move(stats);
    node->scripts.push_back(std::move(entry));
    return scene.add_node(std::move(node));
}

Stats& stats_of(Scene& scene, uint32_t id) {
    return static_cast<Stats&>(*scene.find(id)->scripts.front().instance);
}

ModelDecl model(const char* name, ModelSource from, const char* component = "Stats") {
    ModelDecl decl;
    decl.name = name;
    decl.component = component;
    decl.from = from;
    return decl;
}

void destroy(Scene& scene, uint32_t id) {
    scene.queue_destroy(id);
    scene.flush_destroyed();
}

} // namespace

TEST_CASE("A self Model reads the component on the node the UI is placed on", "[ui_models]") {
    Scene scene;
    TestWorld world;
    (void)spawn(scene, "enemy", 30.0f);
    const uint32_t placed = spawn(scene, "nameplate", 70.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, placed);
    models.add(model("unit", ModelSource::self), scope);
    models.bind();

    REQUIRE(scope.lookup("unit.hp") == UiValue(70.0f));
    REQUIRE(models.searches() == 0); // one node looked up, no walk
}

TEST_CASE("A local Model walks the scene once and then reads in place", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t enemy = spawn(scene, "enemy", 30.0f);
    const uint32_t player = spawn(scene, "player", 80.0f);
    world.give_away(enemy, true);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("player", ModelSource::local), scope);
    for (int frame = 0; frame < 100; ++frame) models.bind();

    REQUIRE(scope.lookup("player.hp") == UiValue(80.0f));
    REQUIRE(models.searches() == 1);

    stats_of(scene, player).hp = 12.0f;
    models.bind();
    REQUIRE(scope.lookup("player.hp") == UiValue(12.0f));

    (void)spawn(scene, "crate", 0.0f); // the scene changes; what was found stands
    models.bind();
    REQUIRE(models.searches() == 1);
}

TEST_CASE("A destroyed node empties its Model, and a respawned one reconnects", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t player = spawn(scene, "player", 80.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("player", ModelSource::local), scope);
    models.bind();
    REQUIRE(scope.evaluate("player").truthy());

    destroy(scene, player);
    for (int frame = 0; frame < 100; ++frame) models.bind();
    REQUIRE_FALSE(scope.evaluate("player").truthy());
    REQUIRE(scope.lookup("player.hp").empty());
    REQUIRE(models.searches() == 2); // once when it went, not once a frame

    const uint32_t again = spawn(scene, "player", 100.0f);
    models.bind();
    REQUIRE(scope.lookup("player.hp") == UiValue(100.0f));
    REQUIRE(models.searches() == 3);
    REQUIRE(again != player);
}

TEST_CASE("A Model follows its component when the node changes hands", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t first = spawn(scene, "first", 10.0f);
    const uint32_t second = spawn(scene, "second", 20.0f);
    world.give_away(second, true);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("player", ModelSource::local), scope);
    models.bind();
    REQUIRE(scope.lookup("player.hp") == UiValue(10.0f));

    world.give_away(first, true);
    world.give_away(second, false);
    models.bind();
    REQUIRE(scope.lookup("player.hp") == UiValue(20.0f));
}

TEST_CASE("A script added to a node already in the scene is found", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t id = scene.add_empty({}, "player");

    UiContext scope;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("player", ModelSource::local), scope);
    models.bind();
    models.bind();
    REQUIRE_FALSE(scope.evaluate("player").truthy());
    REQUIRE(models.searches() == 1);

    ScriptEntry entry;
    entry.type_name = "Stats";
    entry.instance = std::make_unique<Stats>();
    scene.find(id)->scripts.push_back(std::move(entry));
    scene.mark_structure_changed();
    models.bind();
    REQUIRE(scope.evaluate("player").truthy());
}

TEST_CASE("A Model of a component the game lacks is no value", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t placed = spawn(scene, "player", 80.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, placed);
    models.add(model("mana", ModelSource::self, "Mana"), scope);
    models.bind();
    REQUIRE_FALSE(scope.evaluate("mana").truthy());
    REQUIRE(scope.lookup("mana.hp").empty());
}

TEST_CASE("A Controller binds a Model to a node it chose, until it clears it", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t hud = scene.add_empty({}, "hud"); // carries no Stats
    const uint32_t goblin = spawn(scene, "goblin", 15.0f);
    const uint32_t troll = spawn(scene, "troll", 90.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, hud);
    models.add(model("target", ModelSource::self), scope);
    models.bind();
    REQUIRE_FALSE(scope.evaluate("target").truthy()); // nothing targeted

    REQUIRE(models.bind_node(scope, "target", goblin));
    models.bind();
    REQUIRE(scope.lookup("target.hp") == UiValue(15.0f));

    REQUIRE(models.bind_node(scope, "target", troll));
    models.bind();
    REQUIRE(scope.lookup("target.hp") == UiValue(90.0f));

    destroy(scene, troll); // the choice holds, and reads nothing
    models.bind();
    REQUIRE_FALSE(scope.evaluate("target").truthy());

    REQUIRE(models.bind_node(scope, "target", 0)); // back to from="self"
    models.bind();
    REQUIRE_FALSE(scope.evaluate("target").truthy());
    REQUIRE(models.searches() == 0);
}

TEST_CASE("A placement's pick is read over the rule, and a Controller's bind over both", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t hud = scene.add_empty({}, "hud"); // carries no Stats
    const uint32_t goblin = spawn(scene, "goblin", 15.0f);
    const uint32_t troll = spawn(scene, "troll", 90.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, hud);
    models.add(model("target", ModelSource::self), scope, troll);
    models.bind();
    REQUIRE(scope.lookup("target.hp") == UiValue(90.0f));

    REQUIRE(models.bind_node(scope, "target", goblin));
    models.bind();
    REQUIRE(scope.lookup("target.hp") == UiValue(15.0f));

    REQUIRE(models.bind_node(scope, "target", 0)); // back to the placement's pick
    models.bind();
    REQUIRE(scope.lookup("target.hp") == UiValue(90.0f));

    destroy(scene, troll);
    models.bind();
    REQUIRE_FALSE(scope.evaluate("target").truthy());
    REQUIRE(models.searches() == 0);
}

TEST_CASE("Where a Model reads is told by node and component", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t player = spawn(scene, "player", 80.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("player", ModelSource::local), scope);
    models.add(model("mana", ModelSource::local, "Mana"), scope);

    const auto found = models.reading(scope, "player");
    REQUIRE(found.node == player);
    REQUIRE(found.component == static_cast<const void*>(scene.find(player)->scripts.front().instance.get()));
    REQUIRE(models.reading(scope, "mana").node == 0); // the game has no Mana
    REQUIRE(models.reading(scope, "nothing").component == nullptr);
}

TEST_CASE("A tree's widgets bring their Models, a repeat's once it is stamped", "[ui_models]") {
    Scene scene;
    TestWorld world;
    (void)spawn(scene, "player", 50.0f);
    auto widget_node = [] {
        auto node = std::make_unique<UiNode>();
        node->is_widget = true;
        node->scoped_context = std::make_unique<UiContext>();
        node->widget_models = {model("player", ModelSource::local)};
        return node;
    };
    UiNode root;
    root.children.push_back(widget_node());
    auto repeat = std::make_unique<UiNode>();
    repeat->foreach_item_index = 0;
    repeat->children.push_back(widget_node());
    root.children.push_back(std::move(repeat));
    UiContext& placed = *root.children[0]->scoped_context;
    UiNode& stamped = *root.children[1];
    UiContext& repeated = *stamped.children[0]->scoped_context;

    UiModels models;
    models.attach(scene, world, 0);
    models.add_widgets(root);
    models.bind();
    REQUIRE(placed.lookup("player.hp") == UiValue(50.0f));
    REQUIRE(models.reading(repeated, "player").component == nullptr); // left to its stamping

    models.add_widgets(stamped);
    models.bind();
    REQUIRE(repeated.lookup("player.hp") == UiValue(50.0f));

    models.remove_widgets(stamped);
    REQUIRE(models.reading(repeated, "player").component == nullptr);
    REQUIRE(models.reading(placed, "player").component != nullptr);
}

TEST_CASE("Only a Model the widget declares can be bound", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t goblin = spawn(scene, "goblin", 15.0f);

    UiContext scope;
    UiContext other;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("target", ModelSource::self), scope);

    REQUIRE_FALSE(models.bind_node(scope, "focus", goblin));
    REQUIRE_FALSE(models.bind_node(other, "target", goblin)); // another widget's scope
}

TEST_CASE("A Controller reads the component its markup shows, of the declared type", "[ui_models]") {
    Scene scene;
    TestWorld world;
    const uint32_t player = spawn(scene, "player", 80.0f);

    UiContext scope;
    UiModels models;
    models.attach(scene, world, 0);
    models.add(model("player", ModelSource::local), scope);

    const void* read = models.component(scope, "player", "Stats");
    REQUIRE(read == scene.find(player)->scripts.front().instance.get());
    REQUIRE(models.component(scope, "player", "Mana") == nullptr); // another type
    REQUIRE(models.component(scope, "enemy", "Stats") == nullptr);  // undeclared

    destroy(scene, player);
    REQUIRE(models.component(scope, "player", "Stats") == nullptr);
}

TEST_CASE("A widget declares its Models and writes them back", "[ui_models]") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("fjell_models_test_" + std::to_string(platform::process_id()) + ".fjwidget");
    std::ofstream(path) << "<widget>\n"
                           "    <prop name=\"title\" type=\"string\" default=\"HP\"/>\n"
                           "    <model name=\"player\" component=\"PlayerStats\" from=\"local\"/>\n"
                           "    <model name=\"unit\" component=\"Stats\"/>\n"
                           "    <div><text>{title} {player.hp|round}</text></div>\n"
                           "</widget>\n";
    auto asset = load_widget_asset(path.string());
    std::filesystem::remove(path);
    REQUIRE(asset);
    REQUIRE(asset->models.size() == 2);
    REQUIRE(asset->models[0].name == "player");
    REQUIRE(asset->models[0].component == "PlayerStats");
    REQUIRE(asset->models[0].from == ModelSource::local);
    REQUIRE(asset->models[0].source_line == 3);
    REQUIRE(asset->models[1].from == ModelSource::self); // the default

    const std::string text = widget_asset_text(*asset, asset->markup_root.get());
    REQUIRE(text.find("<model name=\"player\" component=\"PlayerStats\" from=\"local\"/>") != std::string::npos);
    REQUIRE(text.find("<model name=\"unit\" component=\"Stats\"/>") != std::string::npos);
}
