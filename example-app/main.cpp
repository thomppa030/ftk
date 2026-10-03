// ftk-example-app, ftk's own test program. Each page shows one thing ftk's UI
// has to get right. It builds twice from this source: ftk-example-app on the
// UI core, with the core's pages, and ftk-example-app-imgui on the kit's
// ImGui implementation (FTK_EXAMPLE_APP_IMGUI), with the gallery of the kit.
// The gallery comes to the core's build with the kit (Phase 6 of the UI core
// plan).
//
//     ftk-example-app [--page <name>]              a page, the first by default
//     ftk-example-app --shot <name> <file.png>     a page drawn off screen at a
//                                                  fixed size, written as a PNG
//     ... --scale <pixels per unit>                a core page at that scale

#ifdef FTK_EXAMPLE_APP_IMGUI
#include "ftk/app/imgui_app_window.hpp"
#include "ftk/ui/kit/gallery.hpp"
#else
#include "painter.hpp"
#include "primitives_page.hpp"
#include "text_page.hpp"

#include "ftk/app/app_window.hpp"
#include "ftk/ui/theme.hpp"
#endif

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
enum class Page { gallery, primitives, text };

struct PageName {
    Page page;
    std::string_view name;
};

constexpr std::array<PageName, 3> PAGES{
    {{Page::gallery, "gallery"}, {Page::primitives, "primitives"}, {Page::text, "text"}}};

/// Whether this build has the page: the gallery is ImGui's, the rest the
/// core's.
constexpr bool built_with(Page page) {
#ifdef FTK_EXAMPLE_APP_IMGUI
    return page == Page::gallery;
#else
    return page != Page::gallery;
#endif
}

/// The page a run starts on when none is named.
#ifdef FTK_EXAMPLE_APP_IMGUI
constexpr Page FIRST = Page::gallery;
#else
constexpr Page FIRST = Page::primitives;
#endif

/// The size a page's shot is drawn at, in layout units: tall enough to show
/// the whole page without scrolling, so a shot covers everything on it.
constexpr glm::uvec2 shot_size(Page page) {
    switch (page) {
    case Page::gallery: return {480, 5000};
    case Page::primitives:
    case Page::text: return {1280, 800};
    }
    return {1280, 800};
}

std::optional<Page> page_named(std::string_view name) {
    for (const PageName& entry : PAGES) {
        if (entry.name != name) continue;
        if (built_with(entry.page)) return entry.page;
        FTK_CORE_ERROR("ftk example app: the page {} is the other build's", name);
        return std::nullopt;
    }
    FTK_CORE_ERROR("ftk example app has no page called {}", name);
    return std::nullopt;
}

/// What the app was asked to do.
struct Request {
    Page page{FIRST};
    /// Where a shot goes; none to run in a window.
    std::optional<std::filesystem::path> shot{};
    /// Pixels to the layout unit, for a page of the core.
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
        } else if (arg == "--scale" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), request.scale);
            if (parsed.ec != std::errc{} || request.scale <= 0.0f) {
                FTK_CORE_ERROR("ftk example app: --scale takes a number above zero, not {}", value);
                return std::nullopt;
            }
        } else {
            FTK_CORE_ERROR("ftk example app does not know {}: use --page <name>, "
                           "--shot <name> <file.png> or --scale <number>",
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

// ftk_stage_editor_data() put the fonts and the ImGui layer's fragment stage
// in ftk/ beside the binary.
std::filesystem::path staged() {
    return ftk::platform::executable_path().parent_path() / "ftk";
}

#ifdef FTK_EXAMPLE_APP_IMGUI

int run(const Request& request) {
    auto made = ftk::ImGuiAppWindow::create({
        .title = "ftk example app (imgui kit)",
        .width = 1280,
        .height = 800,
        .device = {.name = "ftk example app"},
        .files = {.fonts = staged() / "fonts", .srgb_fragment = staged() / "imgui.frag.spv"},
    });
    if (!made) {
        FTK_CORE_CRITICAL("ftk example app could not start: {}", made.error());
        return EXIT_FAILURE;
    }
    ftk::ImGuiAppWindow& app = **made;
    ftk::ui::KitGallery gallery;
    const auto draw = [&](float width, float height) { gallery.draw_page(width, height); };

    if (request.shot) {
        auto shot = app.shot(draw, shot_size(request.page));
        if (!shot) {
            FTK_CORE_CRITICAL("ftk example app could not take the shot: {}", shot.error());
            return EXIT_FAILURE;
        }
        return write_png(*request.shot, *shot) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    auto ran = app.run({.draw = draw});
    if (!ran) {
        FTK_CORE_CRITICAL("ftk example app stopped: {}", ran.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#else

/// Draws the pages of the UI core, one at a time, with tabs at the top right
/// that switch between them in the window.
class CorePages {
public:
    CorePages(ftk::AppWindow& app, const std::filesystem::path& fonts, Page page)
        : fonts_(ftk::example::add_fonts(app.ui().text(), fonts)), primitives_(app, fonts_), text_(fonts_),
          page_(page) {}

    void draw(ftk::ui::core::Context& ui, glm::vec2 size) {
        ui.draw_list().rect_filled({0.0f, 0.0f}, size, ftk::theme::surface_base());
        if (page_ == Page::text) {
            text_.draw(ui, size);
        } else {
            primitives_.draw(ui, size);
        }
        tabs(ui, size);
    }

private:
    void tabs(ftk::ui::core::Context& ui, glm::vec2 size) {
        namespace core = ftk::ui::core;
        ftk::example::Painter p(ui, fonts_);
        constexpr Page SHOWN[] = {Page::primitives, Page::text};
        constexpr std::string_view NAMES[] = {"Primitives", "Text"};
        constexpr float PAD = 14.0f;
        float x = size.x - 32.0f;
        for (int i = 1; i >= 0; --i) x -= p.width(NAMES[i], ftk::theme::BODY_TEXT) + 2.0f * PAD + (i == 1 ? 0.0f : 6.0f);
        for (int i = 0; i < 2; ++i) {
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
            x += width + 6.0f;
        }
    }

    ftk::example::Fonts fonts_;
    ftk::example::PrimitivesPage primitives_;
    ftk::example::TextPage text_;
    Page page_;
};

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
    CorePages pages(app, staged() / "fonts", request.page);
    const ftk::UiDraw ui = [&](ftk::ui::core::Context& context, glm::vec2 size) { pages.draw(context, size); };

    if (request.shot) {
        const glm::uvec2 size = shot_size(request.page);
        auto shot = app.shot(ui, glm::uvec2(glm::vec2(size) * request.scale), request.scale);
        if (!shot) {
            FTK_CORE_CRITICAL("ftk example app could not take the shot: {}", shot.error());
            return EXIT_FAILURE;
        }
        return write_png(*request.shot, *shot) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    auto ran = app.run({.ui = ui});
    if (!ran) {
        FTK_CORE_CRITICAL("ftk example app stopped: {}", ran.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#endif

} // namespace

int main(int argc, char** argv) {
    ftk::log::init();
    const auto request = parse(argc, argv);
    const int code = request ? run(*request) : EXIT_FAILURE;
    ftk::log::shutdown();
    return code;
}
