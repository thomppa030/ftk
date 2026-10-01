#pragma once

#include "ftk/base/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// ── Climate (.fjclimate) ─────────────────────────────────────────────────
//
// What weather a place gets and in what order. A climate is a table of
// states, each naming a `.fjweather` event, how likely it is, the hours it
// may start in, and which states may follow it. The director rolls the next
// state when one ends, so a scene keeps having weather without a script
// deciding each turn, and a designer shapes the distribution rather than the
// sequence: clear leads to overcast, overcast to rain, rain back to
// overcast, never clear straight to a blizzard.
//
// Rolls are deterministic given the climate's seed and how many events have
// played, which is what lets every machine in a session agree on the weather
// from one message.

struct ClimateState {
    /// Name of the state, for the table's `next` lists and the dry run.
    std::string name;
    /// The `.fjweather` this state plays, project-relative.
    std::string event;
    /// How likely this state is among the allowed ones; 0 never rolls.
    float weight{1.0f};
    /// The hours of day this state may start in, as [from, to). A window
    /// that wraps midnight (22 to 4) is allowed. Equal bounds mean any hour.
    float from_hour{0.0f};
    float to_hour{0.0f};
    /// The states that may follow this one, by name. Empty allows any.
    std::vector<std::string> next;

    /// Whether `hour` falls in this state's window.
    [[nodiscard]] bool allows_hour(float hour) const;
};

struct ClimateAsset {
    /// Display name, for the inspector. Defaults to the file stem.
    std::string name;
    std::vector<ClimateState> states;
    /// The state the director starts from; empty starts from the first.
    std::string initial;
    /// Seeds the rolls. Two runs of the same seed get the same weather.
    uint32_t seed{1};

    [[nodiscard]] const ClimateState* find(std::string_view state_name) const;
    [[nodiscard]] int index_of(std::string_view state_name) const;
};

[[nodiscard]] Result<ClimateAsset> load_climate_asset_file(const std::string& path);
[[nodiscard]] Result<> save_climate_asset_file(const std::string& path, const ClimateAsset& asset);

} // namespace fjell
