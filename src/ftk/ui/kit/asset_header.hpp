#pragma once

#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/save_bar.hpp"
#include "ftk/ui/types.hpp"

#include <optional>
#include <string>

// The strip across an asset editor under the context tabs (sheet 7): what
// is being edited and where, whether it has unsaved changes, Revert and
// Save right after that, and the editor's own actions at the right edge. A
// save that failed shows as a callout under it until the next one works. An
// editor that only reads its file says so in a quiet tag in place of the
// unsaved state, Revert and Save, and one whose file can't be used as it is
// says why in a callout of its own there.
//
//     if (auto header = ui::AssetHeaderBar(spec)) {
//         header.begin_actions();
//         ui::same_line();
//         if (ui::action(ui::icon::file, "wood.frag", ui::ButtonKind::Ghost)) open_shader();
//         switch (header.finish()) { ... }
//     }

namespace ftk::ui {

/// The header strip's window, for placing what hangs from it.
inline constexpr const char* ASSET_HEADER_WINDOW = "##AssetHeader";

/// What an editor says under the header of a file it can't use as it is
/// (sheet 5): what is wrong as the title, why, and the button that fixes it,
/// with its icon; no button for no `action`.
struct AssetHeaderNotice {
    Severity severity{Severity::Error};
    std::string title{};
    std::string text{};
    const char* action_icon{nullptr};
    std::string action{};
};

struct AssetHeaderSpec {
    const char* icon{nullptr};  ///< the asset kind's icon
    Color icon_colour{};        ///< its hue
    std::string name{};         ///< the file name, "crate_wood.png", or how many ("3 textures")
    std::string name_tooltip{}; ///< what hovering the name says: which files, when it names several
    std::string folder{};       ///< where it lives, "Props / Crates"
    bool unsaved{false};
    std::string saves{};        ///< the files Save writes, for its tooltip
    std::string error{};        ///< why the last save failed; empty once one works
    /// The editor only reads the file: a Read only tag in place of the
    /// unsaved state, Revert and Save.
    bool read_only{false};
    /// What the editor says of its file under the strip; under a failed
    /// save's callout when there is one.
    std::optional<AssetHeaderNotice> notice{};
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
    /// drawn after ui::same_line().
    void begin_actions();

    /// Ends the strip and draws the callouts under it: a failed save's, then
    /// the notice. Returns what was clicked: Revert or Save, or the failed
    /// save's Try again as a Save.
    SaveAction finish();

    /// Whether finish() saw the notice's button clicked.
    [[nodiscard]] bool notice_pressed() const { return notice_pressed_; }

private:
    const AssetHeaderSpec& spec_;
    bool open_{false};
    float right_start_{0.0f};
    SaveAction action_{SaveAction::None};
    bool notice_pressed_{false};
};

} // namespace ftk::ui
