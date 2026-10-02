// ftk-example-app, ftk's own test program. Each page shows one thing ftk's UI
// has to get right; the gallery of the kit is the first.
//
//     ftk-example-app [--page <name>]              a page, the gallery by default
//     ftk-example-app --shot <name> <file.png>     a page drawn off screen at a
//                                                  fixed size, written as a PNG

#include "ftk/app/app_window.hpp"
#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"
#include "ftk/ui/kit/gallery.hpp"

#include <stb_image_write.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>

namespace {

/// The example app's pages. More arrive with the work that needs them.
enum class Page { gallery };

struct PageName {
    Page page;
    std::string_view name;
};

constexpr std::array<PageName, 1> PAGES{{{Page::gallery, "gallery"}}};

/// The size a page's shot is drawn at: tall enough to show the whole page
/// without scrolling, so a shot covers everything on it.
constexpr glm::uvec2 shot_size(Page page) {
    switch (page) {
    case Page::gallery: return {480, 5000};
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
        } else {
            FTK_CORE_ERROR("ftk example app does not know {}: use --page <name> or "
                           "--shot <name> <file.png>",
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

} // namespace

int main(int argc, char** argv) {
    ftk::log::init();
    const auto request = parse(argc, argv);
    const int code = request ? run(*request) : EXIT_FAILURE;
    ftk::log::shutdown();
    return code;
}
