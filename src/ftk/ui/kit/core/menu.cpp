#include "ftk/ui/kit/menu.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <glm/common.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float ICON_COLUMN = 18.0f;

// The menu being drawn: the popup innermost.
Id current_menu() {
    const Kit& k = kit();
    return k.popup_stack.empty() ? 0 : k.popup_stack.back();
}

// Another entry of the menu is under the mouse: the submenu open from the
// menu closes, as ImGui's does.
void close_submenu() {
    Kit& k = kit();
    const auto open = k.submenus.find(current_menu());
    if (open == k.submenus.end()) return;
    ui().close_popup(open->second);
    k.submenus.erase(open);
}

// `rect` widened by half the item spacing each way, as a selectable is.
core::Rect padded(core::Rect rect, glm::vec2 spacing) {
    const glm::vec2 before = glm::floor(spacing * 0.5f);
    return {.min = rect.min - before, .max = rect.max + (spacing - before)};
}

} // namespace

bool menu_item(const MenuItem& item) {
    const Style& style = kit().style;
    const float height = frame_height();
    const float pad = theme::GAP_M;
    const glm::vec2 label_size = measure(item.label);
    float shortcut_w = 0.0f;
    if (item.shortcut != nullptr) {
        FontScope mono(Font::Mono, theme::SMALL_TEXT);
        shortcut_w = measure(item.shortcut).x;
    }
    // The widest entry sets the menu's width: icon column, label, and the
    // shortcut kept clear of it.
    const float natural = pad + ICON_COLUMN + pad + label_size.x +
                          (shortcut_w > 0.0f ? theme::GAP_L * 2.0f + shortcut_w : 0.0f) + pad;

    const glm::vec2 min = cursor();
    bool chosen = false;
    {
        IdScope scope(item.label);
        Disabled off(!item.enabled);
        chosen = selectable("##entry", false, {.size = {std::max(space_left().x, natural), height}, .name = item.label});
    }
    if (!item.enabled && item.disabled_reason != nullptr) item_tooltip(item.disabled_reason);
    if (item_hovered()) close_submenu();
    const glm::vec2 max = item_max();

    Color colour = item.destructive ? theme::error() : theme::text();
    if (!item.enabled) colour.a *= style.disabled_alpha;
    const float y = min.y + (height - label_size.y) * 0.5f;
    const char* glyph = item.checked ? (*item.checked ? icon::checked : nullptr) : item.icon;
    if (glyph != nullptr) draw_text({min.x + pad, y}, colour, glyph);
    draw_text({min.x + pad + ICON_COLUMN + pad, y}, colour, item.label);
    if (item.shortcut != nullptr) {
        Color dim = theme::text_secondary();
        if (!item.enabled) dim.a *= style.disabled_alpha;
        FontScope mono(Font::Mono, theme::SMALL_TEXT);
        draw_text({max.x - pad - shortcut_w, min.y + (height - font_size()) * 0.5f}, dim, item.shortcut);
    }
    return chosen;
}

bool begin_menu(const char* icon, const char* label, bool enabled) {
    core::Context& context = ui();
    Kit& k = kit();
    const Style& style = k.style;
    const float pad = theme::GAP_M;
    const glm::vec2 label_size = measure(label);
    const float natural = pad + ICON_COLUMN + pad + label_size.x + theme::GAP_L * 2.0f + pad;

    // The entry, as tall as any other, opens its submenu under the mouse and
    // stays lit while it is open.
    const std::string id = std::string("##") + label;
    const Id entry = context.id(id);
    const Id sub = core::hash_label("##submenu", entry);
    const float height = frame_height();
    const core::Rect rect = context.layout().next({std::max(space_left().x, natural), height});
    const core::Rect bb = padded(rect, style.item_spacing);
    core::Interaction in{};
    {
        Disabled off(!enabled);
        in = place(entry, bb);
    }
    context.semantics(entry, core::Role::MenuItem, label, {}, {.expanded = context.popup_open(sub)});
    const Id menu = current_menu();
    if (enabled && (in.hovered || in.clicked) && !context.popup_open(sub)) {
        // Any other submenu of this menu closes; this one opens beside it.
        close_submenu();
        context.open_popup(sub, {context.panel_rect().max.x - 2.0f, bb.min.y - style.window_padding.y});
        k.submenus[menu] = sub;
    }
    const bool open = context.popup_open(sub);

    core::DrawList& dl = context.draw_list();
    if (open || in.hovered) dl.rect_filled(bb.min, bb.max, faded(in.hovered ? theme::surface_hover() : theme::surface_raised()));
    Color colour = theme::text();
    if (!enabled) colour.a *= style.disabled_alpha;
    const float y = rect.min.y + (height - label_size.y) * 0.5f;
    if (icon != nullptr) draw_text({rect.min.x + pad, y}, colour, icon);
    draw_text({rect.min.x + pad + ICON_COLUMN + pad, y}, colour, label);
    // The arrow saying a submenu opens from it, at its right end.
    const float half = std::floor(line_height() * 0.25f);
    const glm::vec2 tip{rect.max.x - pad, rect.min.y + height * 0.5f};
    dl.triangle_filled({tip.x - half * 1.5f, tip.y - half}, {tip.x - half * 1.5f, tip.y + half}, tip, faded(colour));

    return corekit::begin_popup(sub, {.padding = style.window_padding});
}

void menu_heading(const char* label) {
    std::string upper(label);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const float pad = theme::GAP_M;
    reserve({0.0f, theme::GAP_XS});
    const glm::vec2 min = cursor();
    FontScope bold(Font::Bold, theme::SMALL_TEXT - 1.5f);
    const glm::vec2 size = measure(upper);
    draw_text({min.x + pad + ICON_COLUMN + pad, min.y}, theme::text_disabled(), upper);
    reserve({pad + ICON_COLUMN + pad + size.x + pad, size.y + theme::GAP_XS});
}

void menu_title(const char* icon, std::string_view name, std::string_view kind) {
    const float height = frame_height();
    const float pad = theme::GAP_M;
    glm::vec2 name_size{0.0f};
    {
        FontScope bold(Font::Bold, font_size());
        name_size = measure(name);
    }
    const float kind_w = kind.empty() ? 0.0f : theme::GAP_M + measure(kind).x;
    const glm::vec2 min = cursor();
    reserve({pad + ICON_COLUMN + pad + name_size.x + kind_w + pad, height});

    const float y = min.y + (height - name_size.y) * 0.5f;
    const float label_x = min.x + pad + ICON_COLUMN + pad;
    if (icon != nullptr) draw_text({min.x + pad, y}, theme::text_secondary(), icon);
    {
        FontScope bold(Font::Bold, font_size());
        draw_text({label_x, y}, theme::text(), name);
    }
    if (!kind.empty()) draw_text({label_x + name_size.x + theme::GAP_M, y}, theme::text_secondary(), kind);
}

void end_menu() {
    corekit::end_popup();
}

MenuBar::MenuBar() {
    Kit& k = kit();
    k.bar_menu_shown = k.bar_menu_shown_next;
    k.bar_menu_shown_next = 0;
    // Across the top of the program's window, a field's height, its menus
    // in from its left as far as ImGui's are.
    const core::Rect rect = take_edge_bar(true, frame_height());
    next_window_place(rect.min, rect.size());
    open_ = begin_window("##MainMenuBar",
                         {.title_bar = false,
                          .resize = false,
                          .move = false,
                          .collapse = false,
                          .scrollbar = false,
                          .background = false,
                          .docking = false,
                          .nav_focus = false,
                          .remembered = false},
                         {std::max(k.style.window_padding.x, k.style.item_spacing.x), 0.0f});
    const core::Rect shown = ui().area_rect();
    ui().draw_list().rect_filled(shown.min, shown.max, faded(theme::surface_sunken()));
}

MenuBar::~MenuBar() {
    end_window();
}

bool begin_bar_menu(const char* label) {
    core::Context& context = ui();
    Kit& k = kit();
    const Style& style = k.style;
    const glm::vec2 label_size = measure(label);
    const float half = std::floor(style.item_spacing.x * 0.5f);

    // On the bar's line after the menus before it: the name with half the
    // spacing either side, taking the mouse across twice the spacing.
    same_line(0.0f);
    const glm::vec2 at = cursor();
    const core::Rect name{.min = {at.x + half, at.y + style.frame_padding.y},
                          .max = {at.x + half + label_size.x, at.y + style.frame_padding.y + label_size.y}};
    (void)context.layout().next({label_size.x + half * 2.0f, frame_height()});
    const core::Rect bb = padded(name, {style.item_spacing.x * 2.0f, style.item_spacing.y});
    const Id id = context.id(label);
    const Id popup = core::hash_label("##menu", id);
    const core::Interaction in = place(id, bb);
    context.semantics(id, core::Role::MenuItem, label, {}, {.expanded = context.popup_open(popup)});

    // A press opens it, or closes it when it was open; while another of the
    // bar's menus is open, the mouse on this one opens this one instead.
    const bool another = k.bar_menu != 0 && k.bar_menu != popup && context.popup_open(k.bar_menu);
    if (in.pressed) {
        if (k.bar_menu_shown == popup) {
            context.close_popup(popup);
            k.bar_menu = 0;
        } else {
            context.open_popup(popup, {at.x - 1.0f, at.y + frame_height()});
            k.bar_menu = popup;
        }
    } else if (in.hovered && another) {
        context.close_popup(k.bar_menu);
        context.open_popup(popup, {at.x - 1.0f, at.y + frame_height()});
        k.bar_menu = popup;
    }
    const bool open = context.popup_open(popup);
    if (open || in.hovered) {
        context.draw_list().rect_filled(bb.min, bb.max, faded(in.hovered ? theme::surface_hover() : theme::surface_raised()));
    }
    draw_text(name.min, theme::text(), label);

    const bool shown = corekit::begin_popup(popup, {.padding = style.window_padding});
    if (shown) k.bar_menu_shown_next = popup;
    return shown;
}

} // namespace ftk::ui
