// ftk-ico: an SVG drawn into an ICO file, a Windows program's icon, at the
// sizes Windows shows a program at. ftk_app_icon() (cmake/AppIcon.cmake)
// runs it while a program builds.
//
//     ftk-ico <icon.svg> <icon.ico>

#include "ftk/base/file_io.hpp"
#include "ftk/image/ico.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& why) {
    std::fprintf(stderr, "ftk-ico: %s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return fail("usage: ftk-ico <icon.svg> <icon.ico>");
    const std::filesystem::path from = argv[1];
    const std::filesystem::path to = argv[2];
    const auto svg = ftk::read_text(from);
    if (!svg) return fail(svg.error());
    const auto sizes = ftk::image::rasterize_icon(*svg);
    if (!sizes) return fail(from.string() + ": " + sizes.error());
    const std::vector<std::byte> ico = ftk::image::encode_ico(*sizes);
    if (ico.empty()) return fail(from.string() + " can't be made an icon");
    std::filesystem::create_directories(to.parent_path());
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size()));
    if (!out) return fail(to.string() + " can't be written");
    return 0;
}
