#pragma once

#include "environment/climate_asset.hpp"

#include <cstdint>
#include <vector>

namespace fjell {

/// A weather the director has decided on: which state of the climate, and
/// the ordinal that seeded its roll. The ordinal is what a client replays to
/// reach the same weather, so it travels in the network message.
struct ClimateRoll {
    int state_index{-1};
    uint32_t ordinal{0};

    [[nodiscard]] bool valid() const { return state_index >= 0; }
};

/// Decides what weather comes next. Given a climate and the hour, it rolls a
/// state among those the current one allows, weighted, and keeps a forecast
/// of the rolls after that so anything in the game can see the storm coming.
///
/// Every roll is a pure function of the climate's seed and the roll's
/// ordinal, so two machines that agree on the ordinal agree on the weather
/// without sending it. The hour enters a roll as a filter, which means a
/// client whose clock has drifted far enough to change which states are
/// allowed can diverge; the server's state message resets the ordinal and
/// the hour together, which is what keeps that from lasting.
class ClimateDirector {
public:
    /// How many rolls ahead the forecast holds.
    static constexpr uint32_t FORECAST_LENGTH = 3;

    /// Set the climate and start from its initial state. Null clears it.
    void set_climate(const ClimateAsset* climate, float hour);

    /// Resume a climate mid-sequence, as a client joining a session does:
    /// the ordinal names which roll is playing.
    void resume(const ClimateAsset* climate, uint32_t ordinal, float hour);

    /// Roll the next state, as the current event ending calls for. Returns
    /// what to play, or an invalid roll when there is no climate or nothing
    /// the current state allows at this hour.
    [[nodiscard]] ClimateRoll advance(float hour);

    /// The roll playing now.
    [[nodiscard]] const ClimateRoll& current() const { return current_; }
    /// The rolls after it, soonest first. Recomputed from the hour each time
    /// the forecast is asked for, since a state's window depends on it.
    [[nodiscard]] const std::vector<ClimateRoll>& forecast() const { return forecast_; }
    void update_forecast(float hour);

    /// Whether the director decides anything at all.
    [[nodiscard]] bool active() const { return climate_ != nullptr && !held_; }
    [[nodiscard]] const ClimateAsset* climate() const { return climate_; }

    /// Freeze the director: whatever plays keeps playing and nothing new is
    /// rolled until it is let go. For a script that wants the weather to
    /// stay put through a scene.
    void hold(bool held) { held_ = held; }
    [[nodiscard]] bool held() const { return held_; }

    [[nodiscard]] uint32_t ordinal() const { return ordinal_; }

private:
    /// The state `ordinal` picks, following `from` and starting at `hour`.
    [[nodiscard]] int roll_state(int from, uint32_t ordinal, float hour) const;

    const ClimateAsset* climate_{nullptr};
    ClimateRoll current_{};
    std::vector<ClimateRoll> forecast_;
    uint32_t ordinal_{0};
    bool held_{false};
};

} // namespace fjell
