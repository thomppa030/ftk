#pragma once

#include "core/play_snapshot.hpp"

#include <optional>

namespace fjell {

/// State that exists only while play mode is running. Created by
/// PlayController::start(), destroyed by stop().
class PlaySession {
public:
    explicit PlaySession(std::optional<PlaySnapshot> snapshot)
        : snapshot_{std::move(snapshot)} {}

    /// Empty in game mode, where there is no editor scene to go back to.
    [[nodiscard]] const std::optional<PlaySnapshot>& snapshot() const { return snapshot_; }

private:
    std::optional<PlaySnapshot> snapshot_;
};

} // namespace fjell
