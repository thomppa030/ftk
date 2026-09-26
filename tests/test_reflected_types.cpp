#include "scripting/field_serialization.hpp"
#include "scripting/reflected_types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <new>
#include <string>
#include <vector>

// Tables written the way fjreflect generates them for a widget Model: a HUD
// holding a selected slot and a list of slots, a slot with a getter.
namespace {

struct Slot {
    int count = 0;
    std::string name;
    [[nodiscard]] bool empty() const { return name.empty(); }
};

struct Hud {
    Slot selected;
    std::vector<Slot> slots;
    bool calm = false;
};

void slot_empty(const void* object, void* out) {
    *static_cast<bool*>(out) = static_cast<const Slot*>(object)->empty();
}
uint32_t slots_size(const void* v) {
    return static_cast<uint32_t>(static_cast<const std::vector<Slot>*>(v)->size());
}
const void* slots_at(const void* v, uint32_t i) { return &(*static_cast<const std::vector<Slot>*>(v))[i]; }
void slots_insert(void* v, uint32_t i) {
    auto& l = *static_cast<std::vector<Slot>*>(v);
    l.insert(l.begin() + i, Slot{});
}
void slots_erase(void* v, uint32_t i) {
    auto& l = *static_cast<std::vector<Slot>*>(v);
    l.erase(l.begin() + i);
}
void slots_move(void*, uint32_t, uint32_t) {}

FjellFieldDesc field(const char* name, FjellFieldType type, uint32_t offset, uint32_t flags) {
    FjellFieldDesc f{};
    f.name = name;
    f.type = type;
    f.offset = offset;
    f.flags = flags;
    return f;
}

extern const FjellTypeDesc slot_type;

const FjellVectorDesc slots_vector{FJELL_FIELD_STRUCT, &slot_type, sizeof(Slot), slots_size, slots_at,
                                   slots_insert, slots_erase, slots_move};

FjellFieldDesc hud_fields_init[3];
const FjellFieldDesc* hud_fields() {
    hud_fields_init[0] = field("selected", FJELL_FIELD_STRUCT, offsetof(Hud, selected), FJELL_FIELD_FLAG_STATE);
    hud_fields_init[0].type_desc = &slot_type;
    hud_fields_init[1] = field("slots", FJELL_FIELD_VECTOR, offsetof(Hud, slots), FJELL_FIELD_FLAG_STATE);
    hud_fields_init[1].vector = &slots_vector;
    hud_fields_init[2] = field("calm", FJELL_FIELD_BOOL, offsetof(Hud, calm), FJELL_FIELD_FLAG_STATE);
    return hud_fields_init;
}

FjellFieldDesc slot_fields_init[3];
const FjellFieldDesc* slot_fields() {
    slot_fields_init[0] = field("count", FJELL_FIELD_INT, offsetof(Slot, count), FJELL_FIELD_FLAG_STATE);
    slot_fields_init[1] = field("name", FJELL_FIELD_STD_STRING, offsetof(Slot, name), FJELL_FIELD_FLAG_STATE);
    slot_fields_init[2] = field("empty", FJELL_FIELD_BOOL, 0, FJELL_FIELD_FLAG_STATE | FJELL_FIELD_FLAG_GETTER);
    slot_fields_init[2].get = slot_empty;
    return slot_fields_init;
}

const FjellTypeDesc slot_type{"Slot", "game::Slot", sizeof(Slot), alignof(Slot), slot_fields(), 3,
                              [](void* at) { new (at) Slot(); },
                              [](void* at) { static_cast<Slot*>(at)->~Slot(); }};
const FjellTypeDesc hud_type{"Hud", "game::Hud", sizeof(Hud), alignof(Hud), hud_fields(), 3,
                             [](void* at) { new (at) Hud(); },
                             [](void* at) { static_cast<Hud*>(at)->~Hud(); }};

} // namespace

TEST_CASE("Reflected structs resolve each other in any order", "[reflection]") {
    // The HUD comes first and names the slot, registered after it.
    const FjellTypeDesc* table[] = {&hud_type, &slot_type};
    fjell::ReflectedTypes types;
    types.add_types(table, 2);

    const auto* hud = types.find("Hud");
    const auto* slot = types.find("game::Slot");
    REQUIRE(hud != nullptr);
    REQUIRE(slot != nullptr);
    REQUIRE(hud->find_field("selected")->struct_type == slot);
    REQUIRE(hud->find_field("slots")->vector.element_type == slot);
    REQUIRE(hud->find_field("slots")->vector.element == FJELL_FIELD_STRUCT);
    REQUIRE(slot->find_field("empty")->is_getter());
}

TEST_CASE("A vector and a getter are read through their functions", "[reflection]") {
    const FjellTypeDesc* table[] = {&slot_type, &hud_type};
    fjell::ReflectedTypes types;
    types.add_types(table, 2);
    const auto* hud_desc = types.find("Hud");

    Hud hud;
    hud.slots = {{3, "Potion"}, {0, ""}};
    const auto& slots = *hud_desc->find_field("slots");
    const void* list = reinterpret_cast<const char*>(&hud) + slots.offset;
    REQUIRE(slots.vector.size(list) == 2);

    const auto* slot_desc = slots.vector.element_type;
    const void* second = slots.vector.at(list, 1);
    bool empty = false;
    slot_desc->find_field("empty")->get(second, &empty);
    REQUIRE(empty);
    const auto& first_name = *reinterpret_cast<const std::string*>(
        static_cast<const char*>(slots.vector.at(list, 0)) + slot_desc->find_field("name")->offset);
    REQUIRE(first_name == "Potion");

    // The copy edits the list through the module's functions too.
    void* editable = reinterpret_cast<char*>(&hud) + slots.offset;
    slots.vector.insert(editable, 1);
    REQUIRE(hud.slots.size() == 3);
    REQUIRE(hud.slots[1].name.empty());
    slots.vector.erase(editable, 0);
    REQUIRE(hud.slots.size() == 2);
    REQUIRE(hud.slots[0].name.empty());
}

TEST_CASE("A struct the module never registered leaves the field without a type", "[reflection]") {
    fjell::ReflectedTypes types; // nothing added
    std::vector<fjell::OwnedFieldDesc> out;
    types.copy_fields(hud_fields(), 3, "Hud", out);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].struct_type == nullptr);
    REQUIRE(out[1].vector.element_type == nullptr);
}

TEST_CASE("Only properties are saved and loaded", "[reflection]") {
    struct Stats {
        int max_hp = 100;
        int hp = 40;
    };
    FjellFieldDesc table[] = {
        field("max_hp", FJELL_FIELD_INT, offsetof(Stats, max_hp), 0),
        field("hp", FJELL_FIELD_INT, offsetof(Stats, hp), FJELL_FIELD_FLAG_STATE),
        field("low", FJELL_FIELD_BOOL, 0, FJELL_FIELD_FLAG_STATE | FJELL_FIELD_FLAG_GETTER),
    };
    fjell::ReflectedTypes types;
    std::vector<fjell::OwnedFieldDesc> fields;
    types.copy_fields(table, 3, "Stats", fields);
    REQUIRE(fields[0].is_property());
    REQUIRE_FALSE(fields[1].is_property());
    REQUIRE_FALSE(fields[2].is_property());

    Stats stats;
    nlohmann::json saved;
    fjell::serialize_reflected_fields(fields, &stats, saved);
    REQUIRE(saved == nlohmann::json{{"max_hp", 100}});

    Stats loaded;
    fjell::deserialize_reflected_fields(fields, &loaded, nlohmann::json{{"max_hp", 150}, {"hp", 1}});
    REQUIRE(loaded.max_hp == 150);
    REQUIRE(loaded.hp == 40); // runtime state keeps what the code starts it at
}

TEST_CASE("A game pipe is found by name and formats its input", "[reflection]") {
    static const FjellFieldType args[] = {FJELL_FIELD_STD_STRING};
    const FjellPipeDesc pipes[] = {{"coins", FJELL_FIELD_INT, args, 1,
                                    [](const void* input, const void* const* arg, void* out) {
                                        *static_cast<std::string*>(out) =
                                            std::to_string(*static_cast<const int*>(input) / 100) +
                                            *static_cast<const std::string*>(arg[0]);
                                    }}};
    fjell::ReflectedTypes types;
    types.add_pipes(pipes, 1);

    const auto* coins = types.find_pipe("coins");
    REQUIRE(coins != nullptr);
    REQUIRE(coins->input == FJELL_FIELD_INT);
    REQUIRE(coins->arg_types == std::vector<FjellFieldType>{FJELL_FIELD_STD_STRING});
    const int copper = 1250;
    const std::string suffix = " gold";
    const void* arg_ptrs[] = {&suffix};
    std::string text;
    coins->call(&copper, arg_ptrs, &text);
    REQUIRE(text == "12 gold");
    REQUIRE(types.find_pipe("silver") == nullptr);
}
