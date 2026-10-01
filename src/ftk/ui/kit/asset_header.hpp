#pragma once

#include "ftk/ui/kit/save_bar.hpp"

#include <imgui.h>

#include <string>

// The strip across an asset editor under the context tabs (sheet 7): what
// is being edited and where, whether it has unsaved changes, Revert and
// Save right after that, and the editor's own actions at the right edge. A
// save that failed shows as a callout under it until the next one works.
//
//     if (auto header = ui::AssetHeaderBar(spec)) {
//         header.begin_actions();
//         ImGui::SameLine();
//         if (ui::action(ui::icon::shader, "wood_pbr.fjsl", ui::ButtonKind::Ghost)) open_shader();
//         switch (header.finish()) { ... }
//     }

namespace fjell::ui {

/// The header strip's window, for placing what hangs from it.
inline constexpr const char* ASSET_HEADER_WINDOW = "##AssetHeader";

struct AssetHeaderSpec {
    const char* icon{nullptr};  ///< the asset kind's icon
    ImVec4 icon_colour{};       ///< its category colour
    std::string name{};         ///< the file name, "crate_wood.fjmat"
    std::string folder{};       ///< where it lives, "Props / Crates"
    bool unsaved{false};
    std::string saves{};        ///< the files Save writes, for its tooltip
    std::string error{};        ///< why the last save failed; empty once one works
};

/// The strip, pinned under the context tabs for as long as it lives.
class AssetHeaderBar {
public:
    explicit AssetHeaderBar(const AssetHeaderSpec& spec);
    ~AssetHeaderBar();

    AssetHeaderBar(const AssetHeaderBar&) = delete;
    AssetHeaderBar& operator=(const AssetHeaderBar&) = delete;
    AssetHeaderBar(AssetHeaderBar&&) = delete;
    AssetHeaderBar& operator=(AssetHeaderBar&&) = delete;

    explicit operator bool() const { return open_; }

    /// Starts the editor's own actions, against the right edge; each is
    /// drawn after ImGui::SameLine().
    void begin_actions();

    /// Ends the strip and draws the callout for a failed save. Returns what
    /// was clicked: Revert or Save, or the callout's Try again as a Save.
    SaveAction finish();

private:
    const AssetHeaderSpec& spec_;
    bool open_{false};
    float right_start_{0.0f};
    SaveAction action_{SaveAction::None};
};

} // namespace fjell::ui
