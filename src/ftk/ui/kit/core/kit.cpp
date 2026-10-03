#include "ftk/ui/kit/core/kit.hpp"

#include "ftk/base/log.hpp"
#include "ftk/text/face.hpp"
#include "ftk/text/font_chain.hpp"
#include "ftk/text/last_resort.hpp"
#include "ftk/text/system_fallback.hpp"
#include "ftk/ui/core_kit.hpp"

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
    if (auto* kept = dynamic_cast<Kit*>(context.extension())) return *kept;
    auto made = std::make_unique<Kit>();
    Kit& kit = *made;
    context.set_extension(std::move(made));
    // Items as far apart as the kit's style has them.
    context.layout().set_style({.item_spacing = kit.style.item_spacing, .empty_line = theme::BODY_TEXT});
    return kit;
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
    if (ui().disabled()) colour.a *= kit().style.disabled_alpha;
    return colour;
}

Color text_colour() {
    const Kit& k = kit();
    return k.text_colours.empty() ? theme::text() : k.text_colours.back();
}

float item_width() {
    const Kit& k = kit();
    core::Layout& layout = ui().layout();
    if (k.next.width) return std::max(layout.resolve_width(*k.next.width), 1.0f);
    return std::max(std::floor(layout.area().width() * k.style.item_width_share), 1.0f);
}

core::Interaction place(Id id, core::Rect rect, core::Sense sense) {
    Kit& k = kit();
    sense.beneath = true;
    sense.overlap = k.next.allow_overlap;
    k.next = {};
    const core::Interaction in = ui().interact(id, rect, sense);
    k.item = {.id = id, .rect = rect, .takes = true, .in = in};
    return in;
}

void place_still(core::Rect rect) {
    Kit& k = kit();
    k.next = {};
    k.item = {.id = 0, .rect = rect, .takes = false};
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
    const auto chain = [&](std::shared_ptr<const text::Face> primary) {
        return text::FontChain({primary != nullptr ? std::move(primary) : body}, lib.system, resort);
    };
    text::FontChain body_chain = chain(body);
    // The text faces have glyphs of their own where lucide puts its icons.
    if (const auto icons = face(directory, {"lucide.ttf"})) body_chain.claim(icons, 0xE042, 0xE685);

    corekit::Faces& faces = corekit::kit_of(ui).faces;
    faces.body = ui.text().add_font(std::move(body_chain));
    faces.bold = ui.text().add_font(chain(face(directory, {"Geist-Bold.ttf", "Inter-Bold.ttf"})));
    faces.mono = ui.text().add_font(chain(face(directory, {"GeistMono-Regular.ttf", "JetBrainsMono-Regular.ttf"})));
    faces.loaded = true;
    ui.text().set_default_style({.font = faces.body, .size = theme::BODY_TEXT});
    return true;
}

} // namespace ftk::ui
