#pragma once

#include "ui/game_ui/ui_models.hpp"

namespace fjell {

class NetManager;
class ScriptManager;

/// The running game as a UI's Models see it: script components from the
/// loaded module, and who controls which node from the network session.
/// Without a session the local player controls every node, as in a game
/// played alone or a UI previewed in the editor.
class GameModelWorld final : public UiModelWorld {
public:
    GameModelWorld(const ScriptManager& scripts, const NetManager* net) : scripts_{scripts}, net_{net} {}

    [[nodiscard]] std::optional<UiComponentType> component_type(const std::string& name) const override;
    [[nodiscard]] bool owned_by_local(uint32_t node_id) const override;
    [[nodiscard]] uint64_t possession_revision() const override;

private:
    const ScriptManager& scripts_;
    const NetManager* net_;
};

} // namespace fjell
