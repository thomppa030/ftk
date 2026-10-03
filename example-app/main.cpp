// ftk-example-app, ftk's own test program. Each page shows one thing ftk's UI
// has to get right: the UI core's primitives and text, the kit, its gallery,
// the shell's docks, and what the UI costs.
//
//     ftk-example-app [--page <name>]              a page, the first by default
//     ftk-example-app --shot <name> <file.png>     a page drawn off screen at a
//                                                  fixed size, written as a PNG
//     ftk-example-app --measure                    the Performance page's load
//                                                  drawn off screen at each size,
//                                                  what the UI cost printed
//     ... --scale <pixels per unit>                a page at that scale

#include "kit_page.hpp"
#include "painter.hpp"
#include "performance_page.hpp"
#include "primitives_page.hpp"
#include "shell_page.hpp"
#include "text_page.hpp"

#include "ftk/app/app_window.hpp"
#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/gallery.hpp"
#include "ftk/ui/theme.hpp"

#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"

#include <stb_image_write.h>

#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>

namespace {

/// The example app's pages. More arrive with the work that needs them.
enum class Page { gallery, primitives, text, kit, shell, performance };

struct PageName {
    Page page;
    std::string_view name;
};

constexpr std::array<PageName, 6> PAGES{{{Page::gallery, "gallery"},
                                         {Page::primitives, "primitives"},
                                         {Page::text, "text"},
                                         {Page::kit, "kit"},
                                         {Page::shell, "shell"},
                                         {Page::performance, "performance"}}};

/// The page a run starts on when none is named.
constexpr Page FIRST = Page::primitives;

/// The size a page's shot is drawn at, in layout units: tall enough to show
/// the whole page without scrolling, so a shot covers everything on it.
constexpr glm::uvec2 shot_size(Page page) {
    switch (page) {
    case Page::gallery: return {480, 5000};
    case Page::primitives:
    case Page::text:
    case Page::shell:
    case Page::performance: return {1280, 800};
    case Page::kit: return {1280, 2080};
    }
    return {1280, 800};
}

std::optional<Page> page_named(std::string_view name) {
    for (const PageName& entry : PAGES) {
        if (entry.name == name) return entry.page;
    }
    FTK_CORE_ERROR("ftk example app has no page called {}", name);
    return std::nullopt;
}

/// What the app was asked to do.
struct Request {
    Page page{FIRST};
    /// Where a shot goes; none to run in a window.
    std::optional<std::filesystem::path> shot{};
    /// Measure the Performance page's load instead of running.
    bool measure{false};
    /// Pixels to the layout unit.
    float scale{1.0f};
};

std::optional<Request> parse(int argc, char** argv) {
    Request request;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--page" && i + 1 < argc) {
            const auto page = page_named(argv[++i]);
            if (!page) return std::nullopt;
            request.page = *page;
        } else if (arg == "--shot" && i + 2 < argc) {
            const auto page = page_named(argv[++i]);
            if (!page) return std::nullopt;
            request.page = *page;
            request.shot = std::filesystem::path(argv[++i]);
        } else if (arg == "--measure") {
            request.page = Page::performance;
            request.measure = true;
        } else if (arg == "--scale" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), request.scale);
            if (parsed.ec != std::errc{} || request.scale <= 0.0f) {
                FTK_CORE_ERROR("ftk example app: --scale takes a number above zero, not {}", value);
                return std::nullopt;
            }
        } else {
            FTK_CORE_ERROR("ftk example app does not know {}: use --page <name>, "
                           "--shot <name> <file.png>, --measure or --scale <number>",
                           arg);
            return std::nullopt;
        }
    }
    return request;
}

bool write_png(const std::filesystem::path& path, const ftk::Shot& shot) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    const int written = stbi_write_png(path.string().c_str(), static_cast<int>(shot.width),
                                       static_cast<int>(shot.height), 4, shot.pixels.data(),
                                       static_cast<int>(shot.width) * 4);
    if (written == 0) {
        FTK_CORE_ERROR("ftk example app could not write {}", path.string());
        return false;
    }
    FTK_CORE_INFO("ftk example app wrote {}", path.string());
    return true;
}

// ftk_stage_editor_data() put the fonts in ftk/ beside the binary.
std::filesystem::path staged() {
    return ftk::platform::executable_path().parent_path() / "ftk";
}

/// Draws the pages, one at a time, with tabs at the top right that switch
/// between them in the window. A shot of the gallery is the gallery alone.
class CorePages {
public:
    CorePages(ftk::AppWindow& app, const std::filesystem::path& fonts, Page page, bool shot)
        : fonts_(ftk::example::add_fonts(app.ui().text(), fonts)), primitives_(app, fonts_), text_(fonts_),
          performance_(app), page_(page), shot_(shot) {
        // The kit's own faces, for the Kit page and the gallery.
        ftk::ui::use_kit_fonts(app.ui(), fonts);
    }

    /// Before each frame the window draws.
    void update() { performance_.before_frame(); }

    [[nodiscard]] ftk::example::PerformancePage& performance() { return performance_; }

    void draw(ftk::ui::core::Context& ui, glm::vec2 size) {
        ui.draw_list().rect_filled({0.0f, 0.0f}, size, ftk::theme::surface_base());
        if (page_ == Page::text) {
            text_.draw(ui, size);
        } else if (page_ == Page::kit) {
            kit_.draw(size, TABS_BOTTOM);
        } else if (page_ == Page::gallery) {
            gallery_.draw_page(size.x, size.y);
        } else if (page_ == Page::shell) {
            shell_.draw(size, TABS_BOTTOM);
        } else if (page_ == Page::performance) {
            performance_.draw(size, TABS_BOTTOM);
        } else {
            primitives_.draw(ui, size);
        }
        if (!shot_ || page_ != Page::gallery) tabs(ui, size);
    }

private:
    // Where the tabs end, and a page that leaves room for them begins.
    static constexpr float TABS_BOTTOM = 64.0f;

    void tabs(ftk::ui::core::Context& ui, glm::vec2 size) {
        namespace core = ftk::ui::core;
        ftk::example::Painter p(ui, fonts_);
        constexpr Page SHOWN[] = {Page::primitives, Page::text,  Page::kit,
                                  Page::gallery,    Page::shell, Page::performance};
        constexpr std::string_view NAMES[] = {"Primitives", "Text", "Kit", "Gallery", "Shell", "Performance"};
        constexpr int COUNT = 6;
        constexpr float PAD = 14.0f;
        constexpr float GAP = 6.0f;
        float x = size.x - 32.0f + GAP;
        for (int i = 0; i < COUNT; ++i) x -= p.width(NAMES[i], ftk::theme::BODY_TEXT) + 2.0f * PAD + GAP;
        for (int i = 0; i < COUNT; ++i) {
            const float width = p.width(NAMES[i], ftk::theme::BODY_TEXT) + 2.0f * PAD;
            const core::Rect tab{.min = {x, 24.0f}, .max = {x + width, 52.0f}};
            const core::Interaction in = ui.interact(core::hash_int(i, ui.id("##pages")), tab);
            if (in.clicked) page_ = SHOWN[i];
            if (in.hovered) ui.set_cursor_shape(ftk::ui::MouseCursor::Hand);
            const bool on = page_ == SHOWN[i];
            if (on || in.hovered) {
                ui.draw_list().rect_filled(tab.min, tab.max, on ? ftk::theme::surface_active() : ftk::theme::surface_hover(),
                                           14.0f);
            }
            p.body(tab.min + glm::vec2{PAD, 6.0f}, NAMES[i], ftk::theme::BODY_TEXT,
                   on ? ftk::theme::text() : ftk::theme::text_secondary());
            x += width + GAP;
        }
    }

    ftk::example::Fonts fonts_;
    ftk::example::PrimitivesPage primitives_;
    ftk::example::TextPage text_;
    ftk::example::KitPage kit_;
    ftk::ui::KitGallery gallery_;
    ftk::example::ShellPage shell_;
    ftk::example::PerformancePage performance_;
    Page page_;
    bool shot_;
};

/// The Performance page's load drawn off screen at each size: a few frames
/// for the atlas and the layout to settle, then a measured run, each
/// printed.
int measure(ftk::AppWindow& app, CorePages& pages, const ftk::UiDraw& ui, float scale) {
    constexpr int SETTLE = 30;
    constexpr int MEASURED = ftk::example::PerformancePage::HISTORY + 1;
    constexpr const char* SIZES[] = {"1x", "4x", "16x"};
    const glm::uvec2 size = glm::uvec2(glm::vec2(shot_size(Page::performance)) * scale);
    for (int i = 0; i < 3; ++i) {
        pages.performance().set_size(i);
        if (auto settled = app.shot(ui, size, scale, SETTLE); !settled) {
            FTK_CORE_CRITICAL("ftk example app could not draw the load: {}", settled.error());
            return EXIT_FAILURE;
        }
        pages.performance().reset();
        if (auto measured = app.shot(ui, size, scale, MEASURED); !measured) {
            FTK_CORE_CRITICAL("ftk example app could not draw the load: {}", measured.error());
            return EXIT_FAILURE;
        }
        const ftk::example::PerformancePage::Summary s = pages.performance().summary();
        FTK_CORE_INFO("load {:>3}: UI {:.3f} ms mean, {:.3f} p95, {:.3f} max, {} of {} over 1 ms; "
                      "{} primitives; atlas {} of {} pages, {} glyphs",
                      SIZES[i], s.mean_ms, s.p95_ms, s.max_ms, s.over, s.frames, s.primitives, s.atlas_pages,
                      s.atlas_max_pages, s.glyphs);
    }
    return EXIT_SUCCESS;
}

int run(const Request& request) {
    auto made = ftk::AppWindow::create({
        .title = "ftk example app",
        .width = 1280,
        .height = 800,
        .device = {.name = "ftk example app"},
    });
    if (!made) {
        FTK_CORE_CRITICAL("ftk example app could not start: {}", made.error());
        return EXIT_FAILURE;
    }
    ftk::AppWindow& app = **made;
    CorePages pages(app, staged() / "fonts", request.page, request.shot.has_value() || request.measure);
    const ftk::UiDraw ui = [&](ftk::ui::core::Context& context, glm::vec2 size) { pages.draw(context, size); };

    if (request.measure) return measure(app, pages, ui, request.scale);
    if (request.shot) {
        const glm::uvec2 size = shot_size(request.page);
        auto shot = app.shot(ui, glm::uvec2(glm::vec2(size) * request.scale), request.scale);
        if (!shot) {
            FTK_CORE_CRITICAL("ftk example app could not take the shot: {}", shot.error());
            return EXIT_FAILURE;
        }
        return write_png(*request.shot, *shot) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    auto ran = app.run({.ui = ui, .update = [&] { pages.update(); }});
    if (!ran) {
        FTK_CORE_CRITICAL("ftk example app stopped: {}", ran.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    ftk::log::init();
    const auto request = parse(argc, argv);
    const int code = request ? run(*request) : EXIT_FAILURE;
    ftk::log::shutdown();
    return code;
}
