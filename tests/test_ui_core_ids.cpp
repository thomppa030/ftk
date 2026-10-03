#include "ftk/ui/core/id.hpp"
#include "ftk/ui/core/storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace core = ftk::ui::core;
using ftk::ui::Id;

TEST_CASE("A label's id comes from all of it, or from its ### part alone", "[ui-core]") {
    CHECK(core::hash_label("Save", 0) == core::hash_label("Save", 0));
    CHECK(core::hash_label("Save", 0) != core::hash_label("Load", 0));
    // The part after ## tells two Saves apart and isn't shown.
    CHECK(core::hash_label("Save##scene", 0) != core::hash_label("Save##prefab", 0));
    CHECK(core::shown_label("Save##scene") == "Save");
    CHECK(core::shown_label("Save") == "Save");
    CHECK(core::shown_label("##hidden").empty());
    // From ### on, alone: the label can change and the widget stays.
    CHECK(core::hash_label("Play###transport", 0) == core::hash_label("Pause###transport", 0));
    CHECK(core::hash_label("Play###transport", 0) != core::hash_label("Play###other", 0));
    CHECK(core::shown_label("Play###transport") == "Play");
}

TEST_CASE("The same label under different ids is a different widget", "[ui-core]") {
    core::IdStack ids;
    const Id bare = ids.id("Opacity");
    CHECK(bare != 0);

    ids.push("Layer 1");
    const Id first = ids.id("Opacity");
    ids.pop();
    ids.push("Layer 2");
    const Id second = ids.id("Opacity");
    ids.pop();
    CHECK(first != bare);
    CHECK(first != second);
    CHECK(ids.depth() == 0);

    // By index and by address, as a list's rows are.
    ids.push(3);
    const Id third_row = ids.id("Name");
    ids.pop();
    ids.push(4);
    CHECK(ids.id("Name") != third_row);
    ids.pop();
    int a = 0;
    int b = 0;
    CHECK(ids.id(&a) != ids.id(&b));
    CHECK(ids.id(&a) == core::hash_pointer(&a, 0));

    // A literal is hashed as the label it spells, wherever it is stored.
    const std::string copy = "Opacity";
    CHECK(ids.id("Opacity") == ids.id(copy.c_str()));
    CHECK(ids.id("Opacity") == core::hash_label("Opacity", 0));

    // Pushing an id already made is the same as pushing what made it.
    ids.push("Layer 1");
    const Id under_label = ids.id("Opacity");
    ids.pop();
    ids.push_id(core::hash_label("Layer 1", 0));
    CHECK(ids.id("Opacity") == under_label);
    ids.pop();
}

TEST_CASE("Kept state is read back as the kind it was kept as", "[ui-core]") {
    core::Storage storage;
    const Id fold = core::hash_label("fold", 0);
    const Id width = core::hash_label("width", 0);
    CHECK(storage.get_bool(fold, true));
    CHECK(storage.get_int(fold, 7) == 7);

    storage.set_bool(fold, false);
    storage.set_float(width, 120.5f);
    CHECK_FALSE(storage.get_bool(fold, true));
    CHECK(storage.get_float(width) == 120.5f);
    // Another kind under the same id is the fallback, not the bits.
    CHECK(storage.get_int(width, -1) == -1);

    storage.set_int(width, 3);
    CHECK(storage.get_int(width) == 3);
    CHECK(storage.get_float(width, 1.0f) == 1.0f);
    CHECK(storage.size() == 2);
    storage.erase(fold);
    CHECK_FALSE(storage.contains(fold));
}
