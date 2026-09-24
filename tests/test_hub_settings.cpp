#include "core/hub_settings.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>

TEST_CASE("Writing one hub setting keeps the others", "[hub]") {
    const auto dir = std::filesystem::temp_directory_path() / "fjell_hub_settings_test";
    std::filesystem::remove_all(dir);
    const auto path = dir / "hub.json";

    CHECK(fjell::read_hub_setting("recent_projects", path).is_null());   // no file yet

    fjell::write_hub_setting("recent_projects", nlohmann::json::array({{{"path", "/p"}, {"name", "p"}}}), path);
    fjell::write_hub_setting("recent_folders", nlohmann::json::array({"/a", "/b"}), path);

    const auto projects = fjell::read_hub_setting("recent_projects", path);
    REQUIRE(projects.is_array());
    CHECK(projects[0]["name"] == "p");
    CHECK(fjell::read_hub_setting("recent_folders", path).size() == 2);
    std::filesystem::remove_all(dir);
}
