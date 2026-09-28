#include "scene/component_registry.hpp"
#include "scene/preset.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace fjell;

namespace {

// A component holding a node by id, as the UI widget holds the nodes picked
// for its models.
struct Tether {
    uint32_t anchor{0};
};

[[maybe_unused]] const bool tether_registered = [] {
    ComponentRegistry::instance().register_component<Tether>({
        .display_name = "Tether",
        .json_key = "test_tether",
        .init = {},
        .serialize = {},
        .deserialize = {},
        .node_references = [](void* tether, const std::function<void(std::string_view, uint32_t&)>& fn) {
            fn("anchor", static_cast<Tether*>(tether)->anchor);
        },
        .script_type_name = {},
    });
    return true;
}();

void add_script(Scene& scene, uint32_t node, const std::string& type, nlohmann::json fields) {
    ScriptEntry entry;
    entry.type_name = type;
    entry.serialized = std::move(fields);
    scene.find(node)->scripts.push_back(std::move(entry));
}

const nlohmann::json& fields_of(const Scene& scene, uint32_t node) {
    return scene.find(node)->scripts.front().serialized;
}

uint32_t child_of(const Scene& scene, uint32_t parent) {
    for (const auto& node : scene.objects()) {
        if (node->parent_id == static_cast<int32_t>(parent)) return node->id;
    }
    return 0;
}

// A door and the lever under it, which refer to each other: the door's
// script to the lever, the lever's script to both in a list, and the lever's
// tether to the door. The scene also holds a player outside the door.
struct Doorway {
    Scene scene;
    uint32_t player{0};
    uint32_t door{0};
    uint32_t lever{0};

    Doorway() {
        player = scene.add_empty({}, "Player");
        door = scene.add_empty({}, "Door");
        lever = scene.add_empty({}, "Lever");
        scene.find(lever)->parent_id = static_cast<int32_t>(door);
        add_script(scene, door, "DoorController",
                   {{"lever", saved_node_reference(lever)}, {"unset", saved_node_reference(0)}, {"speed", 2.0}});
        add_script(scene, lever, "LeverSwitch",
                   {{"opens", nlohmann::json::array({saved_node_reference(door), saved_node_reference(lever)})}});
        scene.find(lever)->components.add<Tether>().anchor = door;
    }
};

} // namespace

TEST_CASE("A node's references are found in its scripts and its components", "[preset]") {
    Doorway d;
    std::vector<std::string> found;
    d.scene.find(d.lever)->each_node_reference([&](const NodeReferenceSite& site, uint32_t& id) {
        found.push_back(std::string(site.holder) + "." + std::string(site.field) + "=" + std::to_string(id));
    });
    const std::string door = std::to_string(d.door);
    const std::string lever = std::to_string(d.lever);
    CHECK(found == std::vector<std::string>{"LeverSwitch.opens=" + door, "LeverSwitch.opens=" + lever,
                                            "Tether.anchor=" + door});

    // An unset reference is not handed out, and a value is no reference.
    int door_references = 0;
    d.scene.find(d.door)->each_node_reference([&](const NodeReferenceSite&, uint32_t&) { ++door_references; });
    CHECK(door_references == 1);
    CHECK_FALSE(read_node_reference(nlohmann::json{{"node", 3}, {"name", "x"}}));
    CHECK_FALSE(read_node_reference(nlohmann::json{{"asset_path", "a.fjp"}}));
    CHECK(read_node_reference(saved_node_reference(3)) == 3u);
}

TEST_CASE("A preset keeps the references between its own nodes as its own", "[preset]") {
    Doorway d;
    auto preset = PresetTemplate::from_subtree(d.scene, d.door);
    REQUIRE(preset);
    REQUIRE(preset->nodes().size() == 2);
    const Node& door = *preset->nodes()[0];
    const Node& lever = *preset->nodes()[1];
    CHECK(door.scripts.front().serialized["lever"] == saved_node_reference(lever.id));
    CHECK(lever.scripts.front().serialized["opens"] ==
          nlohmann::json::array({saved_node_reference(door.id), saved_node_reference(lever.id)}));
    CHECK(lever.components.get<Tether>()->anchor == door.id);
}

TEST_CASE("Each placed copy of a preset refers to its own nodes", "[preset]") {
    Doorway d;
    auto preset = PresetTemplate::from_subtree(d.scene, d.door);
    REQUIRE(preset);

    Scene level;
    level.add_empty({}, "Ground");
    const uint32_t first = preset->instantiate(level);
    const uint32_t second = preset->instantiate(level);
    for (const uint32_t door : {first, second}) {
        const uint32_t lever = child_of(level, door);
        REQUIRE(lever != 0);
        CHECK(fields_of(level, door)["lever"] == saved_node_reference(lever));
        CHECK(fields_of(level, door)["unset"] == saved_node_reference(0));
        CHECK(fields_of(level, lever)["opens"] ==
              nlohmann::json::array({saved_node_reference(door), saved_node_reference(lever)}));
        CHECK(level.find(lever)->components.get<Tether>()->anchor == door);
    }
    CHECK(child_of(level, first) != child_of(level, second));
    // What was placed leaves the preset as it was.
    CHECK(preset->nodes()[0]->scripts.front().serialized["lever"] == saved_node_reference(preset->nodes()[1]->id));
}

TEST_CASE("A preset that would refer outside itself is refused, naming each reference", "[preset]") {
    Doorway d;
    add_script(d.scene, d.lever, "Alarm", {{"wakes", saved_node_reference(d.player)}});
    d.scene.find(d.door)->components.add<Tether>().anchor = d.player;

    auto preset = PresetTemplate::from_subtree(d.scene, d.door);
    REQUIRE_FALSE(preset);
    CHECK(preset.error() ==
          "'Door' cannot be made a preset, which may not refer to nodes outside it: Tether `anchor` on 'Door' "
          "refers to 'Player'; Alarm `wakes` on 'Lever' refers to 'Player'");

    // The player refers to nothing, so it may be one.
    CHECK(PresetTemplate::from_subtree(d.scene, d.player));
}

TEST_CASE("A loaded preset drops a reference to a node it does not hold", "[preset]") {
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::make_unique<Node3D>());
    nodes.back()->id = 1;
    nodes.back()->name = "Door";
    ScriptEntry entry;
    entry.type_name = "DoorController";
    entry.serialized = {{"self", saved_node_reference(1)}, {"player", saved_node_reference(9)}};
    nodes.back()->scripts.push_back(std::move(entry));

    const PresetTemplate preset = PresetTemplate::from_nodes("Door", std::move(nodes), "Door.fjp");
    const auto& fields = preset.nodes().front()->scripts.front().serialized;
    CHECK(fields["self"] == saved_node_reference(1));
    CHECK(fields["player"] == saved_node_reference(0));
}
