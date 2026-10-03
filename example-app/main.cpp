// ftk-example-app, ftk's own test program. Each page shows one thing ftk's UI
// has to get right; the gallery of the kit is the first. The gallery is drawn
// with ImGui; the pages of the UI core with the core.
//
//     ftk-example-app [--page <name>]              a page, the gallery by default
//     ftk-example-app --shot <name> <file.png>     a page drawn off screen at a
//                                                  fixed size, written as a PNG
//     ... --scale <pixels per unit>                a core page at that scale

#include "primitives_page.hpp"

#include "ftk/app/app_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"
#include "ftk/ui/kit/gallery.hpp"

#include <stb_image_write.h>

#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>

namespace {

/// The example app's pages. More arrive with the work that needs them.
enum class Page { gallery, primitives };

struct PageName {
    Page page;
    std::string_view name;
};

constexpr std::array<PageName, 2> PAGES{{{Page::gallery, "gallery"}, {Page::primitives, "primitives"}}};

/// Whether the page is drawn with the UI core, rather than ImGui.
constexpr bool on_core(Page page) {
    return page == Page::primitives;
}

/// The size a page's shot is drawn at, in layout units: tall enough to show
/// the whole page without scrolling, so a shot covers everything on it.
constexpr glm::uvec2 shot_size(Page page) {
    switch (page) {
    case Page::gallery: return {480, 5000};
    case Page::primitives: return {1280, 800};
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
    Page page{Page::gallery};
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

/// Draws the pages: one at a time, each filling the frame.
class Pages {
public:
    explicit Pages(Page page) : page_(page) {}

    void draw(float width, float height) {
        switch (page_) {
        case Page::gallery: gallery_.draw_page(width, height); break;
        case Page::primitives: break;  // the core's
        }
    }

private:
    Page page_;
    ftk::ui::KitGallery gallery_;
};

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

int run(const Request& request) {
    // ftk_stage_editor_data() put the fonts and the ImGui layer's fragment
    // stage in ftk/ beside the binary.
    const auto staged = ftk::platform::executable_path().parent_path() / "ftk";
    auto made = ftk::AppWindow::create({
        .title = "ftk example app",
        .width = 1280,
        .height = 800,
        .device = {.name = "ftk example app"},
        .files = {.fonts = staged / "fonts", .srgb_fragment = staged / "imgui.frag.spv"},
    });
    if (!made) {
        FTK_CORE_CRITICAL("ftk example app could not start: {}", made.error());
        return EXIT_FAILURE;
    }
    ftk::AppWindow& app = **made;
    Pages pages(request.page);
    const auto draw = [&](float width, float height) { pages.draw(width, height); };
    std::optional<ftk::example::PrimitivesPage> primitives;
    if (request.page == Page::primitives) primitives.emplace(app, staged / "fonts");
    const ftk::UiDraw ui = [&](ftk::ui::core::Context& context, glm::vec2 size) {
        if (primitives) primitives->draw(context, size);
    };

    if (request.shot) {
        const glm::uvec2 size = shot_size(request.page);
        auto shot = on_core(request.page)
                        ? app.shot_ui(ui, glm::uvec2(glm::vec2(size) * request.scale), request.scale)
                        : app.shot(draw, size);
        if (!shot) {
            FTK_CORE_CRITICAL("ftk example app could not take the shot: {}", shot.error());
            return EXIT_FAILURE;
        }
        return write_png(*request.shot, *shot) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    auto ran = on_core(request.page) ? app.run({.ui = ui}) : app.run({.draw = draw});
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
