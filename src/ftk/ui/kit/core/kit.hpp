#pragma once

// The kit on the UI core: what it keeps with the core's context and the
// helpers its pieces share. The kit's headers are the API; each piece of it
// is written here on ftk::ui::core, as the ImGui kit's are written on
// ImGui, and keeps ImGui's model where the kit's API carries it: windows
// by name, the item just drawn, what the next item and the next window
// are to be. Inside ftk-app-ui only.

#include "ftk/ui/core/context.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"
#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftk::ui::corekit {

/// The kit's metrics: those the ImGui kit gives ImGui's style
/// (imgui_theme.cpp), so a piece measures alike on either.
struct Style {
    glm::vec2 window_padding{10.0f, 8.0f};
    glm::vec2 frame_padding{8.0f, 5.0f};
    glm::vec2 item_spacing{8.0f, 6.0f};
    glm::vec2 item_inner_spacing{6.0f, 4.0f};
    float frame_rounding{theme::FRAME_ROUNDING};
    float popup_rounding{4.0f};
    float scrollbar_size{10.0f};
    /// What is drawn in a disabled scope is this opaque.
    float disabled_alpha{0.6f};
    /// A field's width when none is asked for: this share of the room.
    float item_width_share{0.65f};
};

/// The faces the kit draws text in, by role.
struct Faces {
    core::FontId body{0};
    core::FontId bold{0};
    core::FontId mono{0};
    bool loaded{false};
};

/// The item just drawn, for the item queries.
struct Item {
    Id id{0};
    core::Rect rect{};
    /// It took the pointers, and what interact() found; an item that takes
    /// nothing, text or a picture, is hovered by pointer_over().
    bool takes{false};
    core::Interaction in{};
    /// It has the keyboard now, and took it or lost it this frame.
    bool focused{false};
    bool focus_gained{false};
    bool focus_lost{false};
    /// Its value changed while it was active: an edit finished when it
    /// stops.
    bool edited{false};
};

/// What the next item is to be.
struct NextItem {
    std::optional<float> width{};
    bool allow_overlap{false};
    /// Text is a field's height, its line where a field's text would be.
    bool text_on_frame{false};
};

/// What the next window is to be.
struct NextWindow {
    std::optional<glm::vec2> pos{};
    std::optional<glm::vec2> size{};
    std::optional<glm::vec2> size_once{};
    std::optional<std::pair<glm::vec2, glm::vec2>> size_limits{};
};

/// One of the kit's windows: what it keeps from frame to frame.
struct Window {
    std::string name;
    core::Rect rect{};
    /// Sized by the program at least once; until then it fits what it holds.
    bool sized{false};
    /// The frame it was last drawn in.
    uint64_t frame{0};
    WindowOptions options{};
    /// Its padding and title bar, as it began this frame.
    glm::vec2 padding{0.0f};
    float bar{0.0f};
    /// Where it was when its title bar was pressed, to move it from.
    glm::vec2 move_from{0.0f};
    /// Where its scrollbar's grab was pressed, as a share of what scrolls.
    float grab_from{0.0f};
};

class Kit : public core::Context::Extension {
public:
    Style style{};
    Faces faces{};
    Item item{};
    NextItem next{};
    NextWindow next_window{};
    /// The colours text is drawn in, the innermost last; the theme's text
    /// colour beneath them.
    std::vector<Color> text_colours{};
    std::unordered_map<Id, Window> windows{};
    /// The kit's windows being drawn, innermost last, and the one that has
    /// the keyboard.
    std::vector<Id> window_stack{};
    Id focused_window{0};
    /// The popups being drawn, innermost last, for close_popup().
    std::vector<Id> popup_stack{};
    /// The item just drawn before each window, popup or tooltip being drawn
    /// began, which it is again once that ends, as in ImGui.
    std::vector<Item> items_before{};
    /// The drop target being drawn takes drops over this rather than over
    /// the item just drawn: a window that takes them anywhere on it.
    std::optional<core::Rect> drop_rect{};
};

/// The context the kit draws with, the current one, and what the kit keeps
/// in it.
[[nodiscard]] core::Context& ui();
[[nodiscard]] Kit& kit();

/// The label as it is shown: without its "##" part.
[[nodiscard]] std::string_view shown(std::string_view label);

/// The text style text is drawn in now: its face and size, in lines as
/// tall as its size, as the kit lays text out.
[[nodiscard]] core::TextStyle text_style();
/// The room `text` takes in it, wrapped at `wrap`.
[[nodiscard]] glm::vec2 measure(std::string_view text, float wrap = core::NO_WRAP);
/// `text` with its top-left at `at`, in `colour`, faded when disabled.
void draw_text(glm::vec2 at, Color colour, std::string_view text, float wrap = core::NO_WRAP);

/// `colour` as it is drawn now: faded in a disabled scope.
[[nodiscard]] Color faded(Color colour);
/// The colour text is drawn in now.
[[nodiscard]] Color text_colour();

/// The width the next item takes: what next_width() asked, or the share
/// of the room a field takes.
[[nodiscard]] float item_width();

/// Takes `rect` as an item that takes the pointers as `sense` says, placed
/// as ImGui places its items (the first placed on top), and makes it the
/// item just drawn. Returns what the pointers did with it.
core::Interaction place(Id id, core::Rect rect, core::Sense sense = {});
/// Makes `rect` the item just drawn, one that takes nothing.
void place_still(core::Rect rect);
/// Keeps the item just drawn as a window, a popup or a tooltip begins, and
/// makes it the item just drawn again as that ends.
void keep_item();
void restore_item();

/// Begins the kit's window `name`, as ImGui's Begin() does: where it was
/// left or where the next window is asked to be, its background and title
/// bar drawn as `options` say, what it holds `padding` in from its edges.
/// Begun again in a frame, it goes on drawing in it. Ended by end_window(),
/// whatever it returns; false when it shows nothing.
bool begin_window(const char* name, const WindowOptions& options, glm::vec2 padding);
void end_window();

} // namespace ftk::ui::corekit
