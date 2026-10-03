#include "performance_page.hpp"

#include "icons.hpp"

#include "ftk/app/app_window.hpp"
#include "ftk/app/ui_input.hpp"
#include "ftk/gpu/window.hpp"
#include "ftk/text/atlas.hpp"
#include "ftk/ui/core/context.hpp"
#include "ftk/ui/core/text.hpp"
#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/plot.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace ftk::example {

namespace {

/// The UI's CPU time a frame may take on the desktop.
constexpr double TARGET_MS = 1.0;
/// A frame drawn this long after the window's last event, and not at a
/// time the frame before asked for, is drawn while idle.
constexpr double IDLE_AFTER = 1.0;

constexpr std::array<const char*, 3> SIZES{"1×", "4×", "16×"};
/// The load at 1×: the hierarchy's things, the console's lines and the
/// inspector's components.
constexpr int NODES = 150;
constexpr int LINES = 200;
constexpr int COMPONENTS = 4;
constexpr int MOST = 16;

constexpr std::array<const char*, 3> BODIES{"Static", "Dynamic", "Kinematic"};

int times(int size) {
    return size == 0 ? 1 : size == 1 ? 4 : MOST;
}

} // namespace

PerformancePage::PerformancePage(AppWindow& app)
    : app_(app), last_event_(UiInput::now()), things_(static_cast<std::size_t>(COMPONENTS * MOST)) {
    events_ = app_.window().on_event.bind([this](const SDL_Event&) { last_event_ = UiInput::now(); });
    nodes_.reserve(static_cast<std::size_t>(NODES * MOST));
    for (int i = 0; i < NODES * MOST; ++i) nodes_.push_back(std::format("Crate {:04}", i + 1));
    lines_.reserve(static_cast<std::size_t>(LINES * MOST));
    for (int i = 0; i < LINES * MOST; ++i) {
        lines_.push_back(std::format("[12:{:02}:{:02}] Crate {:04} moved to ({:.2f}, 0.00, -2.50)", (i / 60) % 60,
                                     i % 60, i % (NODES * MOST) + 1, (i % 40) * 0.25));
    }
    for (std::size_t i = 0; i < things_.size(); ++i) {
        things_[i].position = {static_cast<float>(i % 8), 0.0f, -2.5f};
        things_[i].rotation = {0.0f, static_cast<float>(i * 15 % 360), 0.0f};
    }
}

void PerformancePage::reset() {
    history_.fill(0.0f);
    next_ = 0;
    filled_ = 0;
    frames_drawn_ = 0;
    idle_frames_ = 0;
}

void PerformancePage::before_frame() {
    const ui::core::FrameOutput* last = app_.layer().last();
    woken_by_time_ = last != nullptr && last->next.at.has_value();
}

PerformancePage::Summary PerformancePage::summary() const {
    Summary out;
    out.frames = filled_;
    if (filled_ > 0) {
        // Since a reset the ring fills from its start.
        std::vector<float> sorted(history_.begin(), history_.begin() + filled_);
        std::ranges::sort(sorted);
        double sum = 0.0;
        for (const float t : sorted) sum += t;
        out.mean_ms = sum / static_cast<double>(sorted.size());
        out.p95_ms = sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
        out.max_ms = sorted.back();
        out.over = static_cast<int>(std::ranges::count_if(sorted, [](float t) { return t > TARGET_MS; }));
    }
    out.primitives = app_.layer().last_cost().primitives;
    const text::GlyphAtlas& atlas = app_.ui().text().atlas();
    out.atlas_pages = atlas.page_count();
    out.atlas_max_pages = atlas.max_pages();
    out.glyphs = atlas.glyph_count();
    return out;
}

void PerformancePage::draw(glm::vec2 size, float top) {
    // The frame before this one, recorded since the page last drew.
    history_[static_cast<std::size_t>(next_)] = static_cast<float>(app_.layer().last_cost().cpu_seconds * 1000.0);
    next_ = (next_ + 1) % HISTORY;
    filled_ = std::min(filled_ + 1, HISTORY);
    ++frames_drawn_;
    if (!redraw_ && !woken_by_time_ && UiInput::now() - last_event_ > IDLE_AFTER) ++idle_frames_;
    if (redraw_) ui::core::current().request_frame();

    using N = ui::DockNode;
    if (ui::begin_plain_window("##performance", {0.0f, top}, {size.x, size.y - top})) {
        draw_toolbar();
        const ui::Id dock = ui::id("##performance_dock");
        ui::dockspace(dock);
        if (!built_) {
            ui::build_dock_tree(
                dock, ui::work_area().size,
                N::left_right(0.16f, N::tabs({"Hierarchy##perf"}),
                              N::left_right(0.7f,
                                            N::left_right(0.67f,
                                                          N::top_bottom(0.62f, N::tabs({"Viewport##perf"}),
                                                                        N::tabs({"Console##perf", "History##perf"})),
                                                          N::tabs({"Inspector##perf"})),
                                            N::tabs({"Performance##perf"}))));
            built_ = true;
        }
    }
    ui::end_plain_window();

    draw_load();
    draw_readouts();
}

void PerformancePage::draw_toolbar() {
    ui::align_text_to_frame();
    ui::text("Load", theme::text_secondary());
    ui::same_line();
    // Room for the three sizes side by side, so they show as segments.
    ui::next_width(150.0f);
    (void)ui::choice("##size", size_, SIZES);
    ui::same_line(theme::GAP_L);
    if (ui::toggle("Redraw every frame", redraw_,
                   "On, a frame is drawn after every frame, to measure the load. Off, frames are drawn "
                   "only when something asks, and one drawn with nothing asking counts as drawn while idle.")) {
        redraw_ = !redraw_;
        last_event_ = UiInput::now();
    }
    ui::same_line();
    if (ui::button("Reset")) reset();
    ui::same_line(theme::GAP_L);
    ui::align_text_to_frame();
    ui::text("UI time: the frame's start to its last command recorded, on the CPU.", theme::text_secondary());
}

void PerformancePage::draw_load() {
    const int n = times(size_);

    if (auto panel = ui::Panel(icon::hierarchy, "Hierarchy##perf")) {
        if (auto tree = ui::Tree("##perf_tree")) {
            const ui::TreeRowResult root = ui::tree_row(
                {.id = "##perf_root", .name = "Harbour", .depth = 0, .has_children = true, .icon = icon::scene});
            if (root.open) {
                for (int i = 0; i < NODES * n; ++i) {
                    const std::string& name = nodes_[static_cast<std::size_t>(i)];
                    (void)ui::tree_row({.id = name.c_str(), .name = name, .depth = 1, .icon = icon::scene});
                }
            }
        }
    }

    if (auto panel = ui::Panel(icon::viewport, "Viewport##perf")) {
        ui::empty_state(icon::scene, "The scene draws here", "The load is the panels around it");
    }

    if (auto panel = ui::Panel(icon::inspector, "Inspector##perf")) {
        for (int i = 0; i < COMPONENTS * n; ++i) {
            ui::IdScope scope(i);
            Thing& thing = things_[static_cast<std::size_t>(i)];
            switch (i % COMPONENTS) {
            case 0:
                ui::section("Transform", nullptr);
                if (auto table = ui::PropertyTable("##rows")) {
                    ui::row("Position", [&] { (void)ui::vec3("##position", thing.position); });
                    ui::row("Rotation", [&] { (void)ui::vec3("##rotation", thing.rotation, {.speed = 1.0f}); });
                    ui::row("Scale", [&] { (void)ui::vec3("##scale", thing.scale, {.speed = 0.01f}); });
                }
                break;
            case 1:
                ui::section("Material", nullptr);
                if (auto table = ui::PropertyTable("##rows")) {
                    ui::row("Roughness", [&] { (void)ui::slider("##roughness", thing.roughness, 0.0f, 1.0f); });
                    ui::row("Metallic", [&] { (void)ui::slider("##metallic", thing.metallic, 0.0f, 1.0f); });
                    ui::row("Cast shadows", [&] { (void)ui::checkbox("##shadows", thing.shadows); });
                }
                break;
            case 2:
                ui::section("Physics", nullptr);
                if (auto table = ui::PropertyTable("##rows")) {
                    ui::row("Body", [&] { (void)ui::choice("##body", thing.body, BODIES); });
                    ui::row("Mass", [&] { (void)ui::drag("##mass", thing.mass, {.speed = 0.1f, .min = 0.0f, .max = 1000.0f}); });
                    ui::row("Friction", [&] { (void)ui::slider("##friction", thing.friction, 0.0f, 1.0f); });
                }
                break;
            default:
                ui::section("Script", nullptr);
                if (auto table = ui::PropertyTable("##rows")) {
                    ui::row("Enabled", [&] { (void)ui::on_off("##enabled", thing.enabled); });
                    ui::row("Speed", [&] { (void)ui::drag("##speed", thing.speed); });
                    ui::row("Steps", [&] { (void)ui::stepper("##steps", thing.steps, 1, 16); });
                }
                break;
            }
        }
    }

    if (auto panel = ui::Panel(ui::icon::console, "Console##perf")) {
        ui::FontScope mono(ui::Font::Mono, theme::SMALL_TEXT);
        for (int i = 0; i < LINES * n; ++i) ui::text(lines_[static_cast<std::size_t>(i)], theme::text_secondary());
    }

    if (auto panel = ui::Panel(ui::icon::history, "History##perf")) {
        ui::text("Move Crate 0412", theme::text_secondary());
        ui::text("Rename Barrel to Crate");
    }
}

void PerformancePage::draw_plot() {
    // The plot runs from 0 to twice the target, so the target is its middle.
    constexpr float TOP_MS = static_cast<float>(TARGET_MS * 2.0);
    ui::plot_lines("##ui_ms", history_, next_, {ui::space_left().x, 64.0f}, {.min = 0.0f, .max = TOP_MS});
    const glm::vec2 pad = ui::frame_padding();
    const glm::vec2 min = ui::item_min() + pad;
    const glm::vec2 max = ui::item_max() - pad;
    ui::DrawList dl = ui::DrawList::window();
    const float y = std::floor((min.y + max.y) * 0.5f) + 0.5f;
    for (float x = min.x; x < max.x; x += 6.0f) {
        dl.line({x, y}, {std::min(x + 3.0f, max.x), y}, theme::text_disabled());
    }
    ui::FontScope small(ui::Font::Current, theme::AXIS_TEXT);
    const std::string scale = std::format("{:.0f} ms", TOP_MS);
    const std::string target = std::format("{:.0f} ms", TARGET_MS);
    dl.text({min.x, min.y - 2.0f}, theme::text_disabled(), scale);
    dl.text({max.x - ui::text_size(target).x, y - ui::text_size(target).y - 1.0f}, theme::text_disabled(), target);
}

void PerformancePage::status_row(const char* label, int count, bool ok) {
    ui::row(label, [&] {
        const ui::Color colour = ok ? theme::success() : theme::error();
        ui::align_text_to_frame();
        const glm::vec2 at = ui::cursor();
        constexpr float DOT = 7.0f;
        ui::DrawList::window().circle_filled({at.x + DOT * 0.5f, at.y + ui::line_height() * 0.5f}, DOT * 0.5f, colour);
        ui::reserve({DOT, ui::line_height()});
        ui::same_line(theme::GAP_S + theme::GAP_XS);
        ui::text(std::format("{}", count), colour);
    });
}

void PerformancePage::draw_readouts() {
    if (auto panel = ui::Panel(icon::performance, "Performance##perf")) {
        const Summary s = summary();
        const float last = history_[static_cast<std::size_t>((next_ + HISTORY - 1) % HISTORY)];

        ui::section("Frame", icon::frame);
        {
            ui::FontScope big(ui::Font::Bold, 22.0f);
            ui::text(std::format("{:.2f}", last));
        }
        ui::same_line();
        ui::align_text_to_frame();
        ui::text("ms of UI last frame", theme::text_secondary());
        draw_plot();
        if (auto table = ui::PropertyTable("##frame")) {
            const auto ms = [](double value) { return std::format("{:.2f} ms", value); };
            ui::row("Mean", [&] { ui::readout(ms(s.mean_ms).c_str()); });
            ui::row("p95", [&] { ui::readout(ms(s.p95_ms).c_str()); });
            ui::row("Max", [&] {
                ui::align_text_to_frame();
                ui::text(ms(s.max_ms), s.max_ms > TARGET_MS ? theme::error() : theme::text_secondary());
            });
            ui::row("Over 1 ms", [&] { ui::readout(std::format("{} of {} frames", s.over, s.frames).c_str()); });
        }

        ui::section("Idle", icon::idle);
        if (auto table = ui::PropertyTable("##idle")) {
            if (redraw_) {
                ui::row("Drawn while idle", [] { ui::readout("not counted while redrawing"); });
            } else {
                status_row("Drawn while idle", idle_frames_, idle_frames_ == 0);
            }
            ui::row("Frames drawn", [&] { ui::readout(std::format("{}", frames_drawn_).c_str()); });
            ui::row("Last event",
                    [&] { ui::readout(std::format("{:.1f} s ago", UiInput::now() - last_event_).c_str()); });
        }

        ui::section("Atlas", icon::atlas);
        if (auto table = ui::PropertyTable("##atlas")) {
            const text::GlyphAtlas& atlas = app_.ui().text().atlas();
            const double page_mb = static_cast<double>(atlas.page_size()) * atlas.page_size() / (1024.0 * 1024.0);
            ui::row("Pages", [&] { ui::readout(std::format("{} of {}", s.atlas_pages, s.atlas_max_pages).c_str()); });
            ui::row("Glyphs", [&] { ui::readout(std::format("{}", s.glyphs).c_str()); });
            ui::row("Memory", [&] {
                ui::readout(
                    std::format("{:.1f} of {:.1f} MB", page_mb * s.atlas_pages, page_mb * s.atlas_max_pages).c_str());
            });
        }

        ui::section("Load", icon::load);
        if (auto table = ui::PropertyTable("##load")) {
            const int n = times(size_);
            ui::row("Hierarchy", [&] { ui::readout(std::format("{} rows", NODES * n + 1).c_str()); });
            ui::row("Inspector", [&] { ui::readout(std::format("{} components", COMPONENTS * n).c_str()); });
            ui::row("Console", [&] { ui::readout(std::format("{} lines", LINES * n).c_str()); });
            ui::row("Primitives", [&] { ui::readout(std::format("{}", s.primitives).c_str()); });
        }
    }
}

} // namespace ftk::example
