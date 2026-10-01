#include "ftk/ui/kit/asset_kind.hpp"
#include "ui/asset_kinds.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("The engine's file types show by their domain once registered", "[ui][assets]") {
    fjell::register_asset_kinds();
    using ftk::theme::Category;
    CHECK(ftk::ui::asset_kind(".fjmat").category == Category::Rendering);
    CHECK(std::string(ftk::ui::asset_kind(".fjmat").noun) == "material");
    CHECK(ftk::ui::asset_kind(".fjweather").category == Category::Environment);
    CHECK(ftk::ui::asset_kind(".fjsurface").category == Category::Physics);
    CHECK(ftk::ui::asset_kind(".fjanim").category == Category::Animation);
    CHECK(std::string(ftk::ui::asset_kind(".fjell").noun) == "scene");
    // The kit's own types are left as they were.
    CHECK(std::string(ftk::ui::asset_kind(".png").noun) == "texture");
}
