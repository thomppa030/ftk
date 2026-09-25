#include "scripting/reflected_types.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_pipes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

using namespace fjell;

namespace {

// A Model as a game would write one, and the descriptors fjreflect would
// generate for it.
struct Stats {
    float hp{40.0f};
    int32_t level{3};
};

struct Player {
    Stats stats;
    std::string name{"Ada"};
    bool alive{true};
    int32_t stance{0}; // an enum: Standing, Crouching
    glm::vec3 position{1.0f, 2.5f, 3.0f};
    char tag[8]{'a', 'b'};

    [[nodiscard]] bool low_health() const { return stats.hp < 25.0f; }
};

uint32_t offset_of(const void* object, const void* member) {
    return static_cast<uint32_t>(static_cast<const char*>(member) - static_cast<const char*>(object));
}

OwnedFieldDesc state(std::string name, FjellFieldType type, uint32_t offset, uint32_t size) {
    OwnedFieldDesc field{};
    field.name = std::move(name);
    field.type = type;
    field.offset = offset;
    field.size = size;
    field.flags = FJELL_FIELD_FLAG_STATE;
    return field;
}

struct PlayerTypes {
    OwnedTypeDesc stats;
    OwnedTypeDesc player;

    PlayerTypes() {
        const Stats s;
        stats.name = "Stats";
        stats.fields = {state("hp", FJELL_FIELD_FLOAT, offset_of(&s, &s.hp), sizeof(float)),
                        state("level", FJELL_FIELD_INT, offset_of(&s, &s.level), sizeof(int32_t))};

        const Player p;
        player.name = "Player";
        auto nested = state("stats", FJELL_FIELD_STRUCT, offset_of(&p, &p.stats), sizeof(Stats));
        nested.struct_type = &stats;
        auto stance = state("stance", FJELL_FIELD_ENUM, offset_of(&p, &p.stance), 2);
        stance.enum_labels = {"Standing", "Crouching"};
        auto low = state("low_health", FJELL_FIELD_BOOL, 0, sizeof(bool));
        low.flags |= FJELL_FIELD_FLAG_GETTER;
        low.get = [](const void* self, void* out) {
            *static_cast<bool*>(out) = static_cast<const Player*>(self)->low_health();
        };
        player.fields = {nested,
                         state("name", FJELL_FIELD_STD_STRING, offset_of(&p, &p.name), sizeof(std::string)),
                         state("alive", FJELL_FIELD_BOOL, offset_of(&p, &p.alive), sizeof(bool)),
                         stance,
                         state("position", FJELL_FIELD_VEC3, offset_of(&p, &p.position), sizeof(glm::vec3)),
                         state("tag", FJELL_FIELD_STRING, offset_of(&p, &p.tag), sizeof(p.tag)),
                         low};
    }

    [[nodiscard]] UiValue object(const Player* p) const { return UiValue(UiObject{player.name, player.fields, p}); }
};

std::string show(const std::string& tmpl, const UiContext& ctx) { return UiDataBind::resolve_template(tmpl, ctx); }

} // namespace

TEST_CASE("A path reads fields through nested structs", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    UiContext ctx;
    ctx.set("player", types.object(&player));

    REQUIRE(ctx.lookup("player.stats.hp") == UiValue(40.0f));
    REQUIRE(ctx.lookup("player.stats.level") == UiValue(3));
    REQUIRE(show("{player.name} is {player.alive}", ctx) == "Ada is 1");
}

TEST_CASE("A path reads the object as it is now", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    UiContext ctx;
    ctx.set("player", types.object(&player));

    REQUIRE(show("{player.stats.hp}", ctx) == "40");
    player.stats.hp = 12.5f;
    player.name = "Grace";
    REQUIRE(show("{player.stats.hp}", ctx) == "12.5");
    REQUIRE(show("{player.name}", ctx) == "Grace");
}

TEST_CASE("A getter in a path runs on every read", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    UiContext ctx;
    ctx.set("player", types.object(&player));

    REQUIRE_FALSE(ctx.evaluate("player.low_health").truthy());
    player.stats.hp = 10.0f;
    REQUIRE(ctx.evaluate("player.low_health").truthy());
}

TEST_CASE("Enums read as labels, char buffers to their end, vectors by component", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    UiContext ctx;
    ctx.set("player", types.object(&player));

    REQUIRE(show("{player.stance}", ctx) == "Standing");
    player.stance = 1;
    REQUIRE(show("{player.stance}", ctx) == "Crouching");
    player.stance = 7; // past the labels
    REQUIRE(show("{player.stance}", ctx) == "7");

    REQUIRE(show("{player.tag}", ctx) == "ab");

    REQUIRE(show("{player.position}", ctx) == "1, 2.5, 3");
    REQUIRE(ctx.lookup("player.position.y") == UiValue(2.5f));
    REQUIRE(ctx.lookup("player.position.b") == UiValue(3.0f));
    REQUIRE(ctx.lookup("player.position.w").empty()); // a vec3 has no fourth
}

TEST_CASE("A path that reaches nothing is no value", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    UiContext ctx;
    ctx.set("player", types.object(&player));
    ctx.set("nobody", types.object(nullptr)); // a Model with nothing found

    REQUIRE(ctx.lookup("player.mana").empty());
    REQUIRE(ctx.lookup("player.stats.hp.x").empty()); // a float has no members
    REQUIRE(ctx.lookup("ghost.stats.hp").empty());

    REQUIRE(ctx.evaluate("player").truthy());
    REQUIRE_FALSE(ctx.evaluate("nobody").truthy());
    REQUIRE(ctx.lookup("nobody.stats.hp").empty());
    REQUIRE(show("[{nobody.name}]", ctx) == "[]");
}

TEST_CASE("A dotted name no scope holds the start of is read whole", "[ui_paths]") {
    UiContext ctx;
    ctx.set("hud.title", "Inventory");
    REQUIRE(show("{hud.title}", ctx) == "Inventory");

    ctx.set("hud", "text"); // now `hud` is a value, and text has no members
    REQUIRE(ctx.lookup("hud.title").empty());
}

TEST_CASE("A widget's scope reads a Model its parent holds, through pipes", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    player.stats.hp = 71.6f;
    UiPipes pipes;
    UiContext root;
    root.set_pipes(&pipes);
    root.set("player", types.object(&player));
    const UiContext widget(&root);

    REQUIRE(show("{player.stats.hp|round}", widget) == "72");
}

TEST_CASE("A prop bound to a struct receives the object itself", "[ui_paths]") {
    const PlayerTypes types;
    Player player;

    auto root = std::make_unique<UiNode>();
    root->type = UiNodeType::element;
    auto widget = std::make_unique<UiNode>();
    widget->type = UiNodeType::element;
    widget->is_widget = true;
    widget->scoped_context = std::make_unique<UiContext>();
    widget->widget_props = {{"stats", "Stats", ""}};
    widget->prop_map["stats"] = "{player.stats}";
    auto label = std::make_unique<UiNode>();
    label->type = UiNodeType::text;
    label->text_template = "HP {stats.hp}";
    auto* shown = label.get();
    widget->children.push_back(std::move(label));
    root->children.push_back(std::move(widget));

    UiContext ctx;
    ctx.set("player", types.object(&player));
    UiDataBind bind;
    bind.resolve(*root, ctx);
    REQUIRE(shown->text_content == "HP 40");
}

TEST_CASE("Objects are the same value only at the same address", "[ui_paths]") {
    const PlayerTypes types;
    Player first;
    Player second;
    REQUIRE(types.object(&first) == types.object(&first));
    REQUIRE_FALSE(types.object(&first) == types.object(&second));
}

TEST_CASE("A scope reads its own object after its values and before its parent", "[ui_paths]") {
    const PlayerTypes types;
    Player player;
    UiContext root;
    UiContext widget(&root);
    widget.set_self(*types.object(&player).object()); // the widget's controller

    root.set("name", "Root");
    root.set("score", UiValue(7));
    REQUIRE(widget.get("name") == "Ada");               // the controller's field, before the parent
    REQUIRE(widget.lookup("stats.hp") == UiValue(40.0f)); // and a path through it
    REQUIRE(widget.lookup("score") == UiValue(7));       // what the controller lacks, from the parent

    player.name = "Grace";
    REQUIRE(widget.get("name") == "Grace"); // read where it is, not copied

    widget.set("name", "Prop");
    REQUIRE(widget.get("name") == "Prop"); // a value published here, before the controller
}
