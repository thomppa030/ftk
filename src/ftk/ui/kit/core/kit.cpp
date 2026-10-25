#include "ftk/ui/kit/core/kit.hpp"

#include "ftk/base/log.hpp"
#include "ftk/text/face.hpp"
#include "ftk/text/font_chain.hpp"
#include "ftk/text/last_resort.hpp"
#include "ftk/text/system_fallback.hpp"
#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ftk::ui {

namespace corekit {

namespace {

Kit& kit_of(core::Context& context) {
    // The last kit found, by the extension it is: every piece asks for the
    // kit many times, and a cast each time was a long list's cost.
    thread_local const core::Context::Extension* last = nullptr;
    thread_local Kit* last_kit = nullptr;
    const core::Context::Extension* extension = context.extension();
    if (extension != nullptr && extension == last) return *last_kit;
    if (auto* kept = dynamic_cast<Kit*>(context.extension())) {
        last = extension;
        last_kit = kept;
        return *kept;
    }
    auto made = std::make_unique<Kit>();
    Kit& kit = *made;
    context.set_extension(std::move(made));
    last = context.extension();
    last_kit = &kit;
    // Items as far apart as the kit's style has them.
    context.layout().set_style({.item_spacing = kit.style.item_spacing, .empty_line = theme::BODY_TEXT});
    return kit;
}

// The live rows, this frame's begun when the frame is.
LiveRow& live_rows(Kit& k) {
    const uint64_t frame = ui().frame();
    if (k.live_row.frame != frame) {
        k.live_row.last = k.live_row.now;
        k.live_row.now = 0;
        k.live_row.frame = frame;
    }
    return k.live_row;
}

} // namespace

core::Context& ui() {
    return core::current();
}

Kit& kit() {
    return kit_of(core::current());
}

std::string_view shown(std::string_view label) {
    const std::size_t hidden = label.find("##");
    return hidden == std::string_view::npos ? label : label.substr(0, hidden);
}

core::TextStyle text_style() {
    core::TextStyle style = ui().text().style();
    style.line_height = style.size;
    return style;
}

glm::vec2 measure(std::string_view text, float wrap) {
    return ui().text().measure(text, text_style(), wrap);
}

void draw_text(glm::vec2 at, Color colour, std::string_view text, float wrap) {
    core::Context& context = ui();
    context.text().draw(context.draw_list(), at, faded(colour), text, text_style(), wrap);
}

Color faded(Color colour) {
    const Style& style = kit().style;
    colour.a *= style.alpha;
    if (ui().disabled()) colour.a *= style.disabled_alpha;
    return colour;
}

std::string ellipsized(std::string_view text, float width) {
    if (measure(text).x <= width) return std::string(text);
    constexpr std::string_view ELLIPSIS = "\xe2\x80\xa6";
    const float room = std::max(width - measure(ELLIPSIS).x, 0.0f);
    // The longest start that fits, a character at a time back from where
    // the caret nearest the room's end goes, the spaces it ends with dropped.
    std::size_t end = std::min(ui().text().caret_at(text, text_style(), room), text.size());
    while (end > 0 && measure(text.substr(0, end)).x > room) {
        do {
            --end;
        } while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u);
    }
    while (end > 0 && text[end - 1] == ' ') --end;
    return std::string(text.substr(0, end)).append(ELLIPSIS);
}

TableState* current_table() {
    Kit& k = kit();
    if (k.table_stack.empty()) return nullptr;
    return &k.tables[k.table_stack.back()];
}

Color text_colour() {
    const Kit& k = kit();
    return k.text_colours.empty() ? theme::text() : k.text_colours.back();
}

FrameColours frame_colours() {
    const Kit& k = kit();
    if (!k.frames.empty()) return k.frames.back();
    return {.rest = theme::surface_sunken(), .hover = theme::surface_base(), .active = theme::surface_raised()};
}

void draw_frame(core::Rect rect, const core::Interaction& in, bool active) {
    const FrameColours colours = frame_colours();
    const Color fill = active || in.held ? colours.active : (in.hovered ? colours.hover : colours.rest);
    ui().draw_list().rect_filled(rect.min, rect.max, faded(fill), kit().style.frame_rounding);
}

void draw_check_box(core::Rect box, const core::Interaction& in, bool on, bool mixed) {
    core::DrawList& dl = ui().draw_list();
    const float side = box.width();
    draw_frame(box, in, false);
    if (mixed) {
        // A dash: some on and some off.
        const float pad = std::max(1.0f, std::floor(side / 4.0f));
        const float thickness = std::max(std::floor(side / 7.0f), 1.0f);
        const float y = std::floor(box.min.y + side * 0.5f - thickness * 0.5f);
        dl.rect_filled({box.min.x + pad, y}, {box.max.x - pad, y + thickness}, faded(text_colour()));
    } else if (on) {
        // ImGui's tick: a short stroke down and a long one up.
        const float pad = std::max(1.0f, std::floor(side / 6.0f));
        const float sz = side - pad * 2.0f;
        const float thickness = std::max(sz / 5.0f, 1.0f);
        const float inner = sz - thickness * 0.5f;
        const glm::vec2 at = box.min + glm::vec2{pad + thickness * 0.25f, pad + thickness * 0.25f};
        const float third = inner / 3.0f;
        const glm::vec2 points[] = {{at.x, at.y + inner - third},
                                    {at.x + third, at.y + inner},
                                    {at.x + third * 3.0f, at.y + inner - third * 3.0f}};
        dl.polyline(points, faded(text_colour()), thickness);
    }
}

float item_width() {
    const Kit& k = kit();
    core::Layout& layout = ui().layout();
    if (k.next.width) return std::max(layout.resolve_width(*k.next.width), 1.0f);
    if (!k.item_widths.empty()) return std::max(layout.resolve_width(k.item_widths.back()), 1.0f);
    return std::max(std::floor(layout.area().width() * k.style.item_width_share), 1.0f);
}

float line_text_drop() {
    const Kit& k = kit();
    float drop = ui().layout().continues_line() ? k.line_drop : 0.0f;
    if (k.next.text_on_frame) drop = std::max(drop, k.style.frame_padding.y);
    return drop;
}

namespace {

// The text drop of the line the item just taken from the layout is on: the
// line's so far when it went on after another, with its own; `field` for an
// item a field's height, whose text is a field's.
void keep_line_drop(Kit& k, bool field) {
    const float own = k.next.text_on_frame || field ? k.style.frame_padding.y : 0.0f;
    k.line_drop = ui().layout().last_continued_line() ? std::max(k.line_drop, own) : own;
}

} // namespace

core::Interaction place(Id id, core::Rect rect, core::Sense sense) {
    Kit& k = kit();
    sense.beneath = true;
    sense.overlap = k.next.allow_overlap;
    sense.keeps_keyboard = sense.keeps_keyboard || k.keeps_keyboard > 0;
    keep_line_drop(k, std::abs(rect.height() - frame_height()) < 0.5f);
    k.next = {};
    const core::Interaction in = ui().interact(id, rect, sense);
    k.item = {.id = id, .rect = rect, .takes = true, .in = in};
    if (in.held) ++k.holds_placed;
    // What a pointer holds or the keyboard is in keeps its row drawn.
    if (k.row_drawn != 0 && (in.held || ui().focused() == id)) live_rows(k).now = k.row_drawn;
    return in;
}

void place_still(core::Rect rect) {
    Kit& k = kit();
    keep_line_drop(k, false);
    k.next = {};
    k.item = {.id = 0, .rect = rect, .takes = false};
}

bool out_of_clip(core::Rect rect) {
    const core::DrawList& dl = ui().draw_list();
    const glm::vec2 clip_min = dl.clip_min();
    const glm::vec2 clip_max = dl.clip_max();
    return rect.max.y <= clip_min.y || rect.min.y >= clip_max.y || rect.max.x <= clip_min.x ||
           rect.min.x >= clip_max.x;
}

bool can_skip_row(Id row, core::Rect rect) {
    return out_of_clip(rect) && live_rows(kit()).last != row;
}

Id live_row() {
    return live_rows(kit()).now;
}

void keep_item() {
    Kit& k = kit();
    k.items_before.push_back(k.item);
}

void restore_item() {
    Kit& k = kit();
    if (k.items_before.empty()) return;
    k.item = k.items_before.back();
    k.items_before.pop_back();
}

namespace {

// The layout's spacing as the kit's style has it.
void lay_out_as(const Style& style) {
    core::LayoutStyle layout = ui().layout().style();
    layout.item_spacing = style.item_spacing;
    ui().layout().set_style(layout);
}

// The edge bars, the last frame's moved aside on a frame's first use.
EdgeBars& edge_bars() {
    EdgeBars& bars = kit().bars;
    const uint64_t frame = ui().frame();
    if (bars.frame != frame) {
        const bool last = bars.frame + 1 == frame;
        bars.last_top = last ? bars.top : 0.0f;
        bars.last_bottom = last ? bars.bottom : 0.0f;
        bars.top = 0.0f;
        bars.bottom = 0.0f;
        bars.frame = frame;
    }
    return bars;
}

} // namespace

void push_style(const Style& style) {
    Kit& k = kit();
    k.styles.push_back(k.style);
    k.style = style;
    lay_out_as(style);
}

void pop_style() {
    Kit& k = kit();
    if (k.styles.empty()) return;
    k.style = k.styles.back();
    k.styles.pop_back();
    lay_out_as(k.style);
}

core::Rect take_edge_bar(bool top, float height) {
    EdgeBars& bars = edge_bars();
    const glm::vec2 root = ui().root_size();
    if (top) {
        const core::Rect rect{.min = {0.0f, bars.top}, .max = {root.x, bars.top + height}};
        bars.top += height;
        return rect;
    }
    const core::Rect rect{.min = {0.0f, root.y - bars.bottom - height}, .max = {root.x, root.y - bars.bottom}};
    bars.bottom += height;
    return rect;
}

core::Rect work_rect() {
    const EdgeBars& bars = edge_bars();
    const glm::vec2 root = ui().root_size();
    return {.min = {0.0f, bars.last_top}, .max = {root.x, std::max(root.y - bars.last_bottom, bars.last_top)}};
}

core::Rect close_rect(glm::vec2 at) {
    return {.min = at, .max = at + glm::vec2{text_style().size}};
}

void draw_close(core::Rect rect, const core::Interaction& in) {
    core::DrawList& dl = ui().draw_list();
    if (in.hovered) {
        dl.rect_filled(rect.min, rect.max, faded(in.held ? theme::surface_active() : theme::surface_highest()));
    }
    const glm::vec2 centre = rect.centre();
    const float extent = rect.width() * 0.5f * 0.7071f - 1.0f;
    const Color ink = faded(text_colour());
    dl.line(centre + glm::vec2{extent, extent}, centre - glm::vec2{extent, extent}, ink, 1.0f);
    dl.line(centre + glm::vec2{extent, -extent}, centre + glm::vec2{-extent, extent}, ink, 1.0f);
}

bool close_button(Id id, glm::vec2 at) {
    const core::Rect rect = close_rect(at);
    const core::Interaction in = ui().interact(id, rect, {.beneath = true});
    draw_close(rect, in);
    return in.clicked;
}

} // namespace corekit

namespace {

// The faces read so far, by file, and the system's fallback: every context
// the program makes shares them.
struct Library {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const text::Face>> faces;
    std::shared_ptr<text::SystemFallback> system;
};

Library& library() {
    // Never destroyed: its faces would go after the font library that made
    // them, whose own state is gone by then at exit.
    static auto* kept = new Library;
    return *kept;
}

// The first of `files` in `directory` that reads, or none.
std::shared_ptr<const text::Face> face(const std::filesystem::path& directory,
                                       std::initializer_list<const char*> files) {
    Library& lib = library();
    for (const char* file : files) {
        const std::filesystem::path path = directory / file;
        if (const auto kept = lib.faces.find(path.string()); kept != lib.faces.end()) return kept->second;
        if (!std::filesystem::exists(path)) continue;
        auto loaded = text::Face::load(path);
        if (!loaded) {
            FTK_CORE_ERROR("UI: {}", loaded.error());
            continue;
        }
        lib.faces.emplace(path.string(), *loaded);
        return *loaded;
    }
    return nullptr;
}

} // namespace

bool use_kit_fonts(core::Context& ui, const std::filesystem::path& directory) {
    Library& lib = library();
    const std::scoped_lock lock(lib.mutex);
    const auto body = face(directory, {"Geist-Medium.ttf", "Inter-Medium.ttf", "Inter-Regular.ttf"});
    if (!body) {
        FTK_CORE_ERROR("UI: the kit's body face isn't in {}", directory.string());
        return false;
    }
    // Japanese where a script is shared, for the fallback's choice.
    if (!lib.system) lib.system = std::make_shared<text::SystemFallback>("ja");
    const auto resort = text::last_resort();
    // Each face draws lucide's icons, as ImGui's fonts have them merged in:
    // the text faces have glyphs of their own where lucide puts its icons.
    // Centred on the text's middle, where ImGui nudges them down by hand.
    const auto icons = face(directory, {"lucide.ttf"});
    // A size is the face's height from ascent to descent, as ImGui sizes
    // its fonts: the editor's text as it was drawn on ImGui (Kay,
    // 2026-10-03), Geist's em 10.8 pixels at the body's 14.
    const auto chain = [&](std::shared_ptr<const text::Face> primary) {
        text::FontChain made({primary != nullptr ? std::move(primary) : body}, lib.system, resort);
        if (icons) made.claim(icons, 0xE042, 0xE685, text::ClaimSeat::Centred);
        made.size_by(text::SizeBasis::Height);
        return made;
    };

    corekit::Faces& faces = corekit::kit_of(ui).faces;
    faces.body = ui.text().add_font(chain(body));
    faces.bold = ui.text().add_font(chain(face(directory, {"Geist-Bold.ttf", "Inter-Bold.ttf"})));
    faces.italic = ui.text().add_font(chain(face(directory, {"Geist-MediumItalic.ttf"})));
    // JetBrains Mono, not Geist Mono: its ligatures keep a cell for each
    // character, where Geist Mono's draw "->" in one and pull the rest of
    // the line off the grid.
    faces.mono = ui.text().add_font(chain(face(directory, {"JetBrainsMono-Regular.ttf"})));
    faces.loaded = true;
    ui.text().set_default_style({.font = faces.body, .size = theme::BODY_TEXT});
    return true;
}

} // namespace ftk::ui
