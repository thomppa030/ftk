// fjlint: reports every problem the engine would report when loading
// .fjss stylesheets and .fjwidget files, without starting the engine.
//
//   fjlint <file or directory>...
//
// Directories are searched for *.fjss and *.fjwidget. A widget's own
// stylesheet is checked along with it. Exit status is 1 when anything was
// reported, so a project's UI can be checked in CI or a pre-commit hook.
//
// Reports use the compiler convention "file:line:column: severity: message",
// which editors parse as is. For Neovim with nvim-lint:
//
//   require("lint").linters.fjlint = {
//     cmd = "fjlint", stdin = false, args = {}, stream = "stdout",
//     ignore_exitcode = true,
//     parser = require("lint.parser").from_errorformat(
//       "%f:%l:%c: %t%*[^:]: %m", { source = "fjlint" }),
//   }
//   require("lint").linters_by_ft.fjss = { "fjlint" }
//
// with a filetype rule mapping *.fjss to "fjss". Vim's own :make works with
// `set makeprg=fjlint\ %` and the default errorformat.

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fjell;

/// Warnings and errors logged so far, from log_stub.cpp.
int fjlint_logged_problems();

namespace {

/// Files checked; problems are counted where they are logged, since the
/// parsers and the widget loader all report through the log.
struct Tally {
    int files{0};
    int problems{0}; ///< only problems the tool finds itself: unreadable or unknown files
};

void check_stylesheet(const fs::path& path, Tally& tally) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "%s: cannot read\n", path.string().c_str());
        ++tally.problems;
        return;
    }
    std::ostringstream text;
    text << file.rdbuf();
    // The parser logs each diagnostic as it finishes; the log sink counts.
    (void)FjssParser::parse(text.str(), path.string());
    ++tally.files;
}

void check_widget(const fs::path& path, Tally& tally) {
    auto asset = load_widget_asset(path.string());
    ++tally.files;
    if (!asset) {
        FJELL_CORE_ERROR("{}: {}", path.string(), asset.error());
        return;
    }
    if (!asset->markup_root) {
        FJELL_CORE_WARN("{}: widget has no markup", path.string());
    }
    if (!asset->fjss_path.empty()) {
        auto sheet_path = path.parent_path() / fs::path(asset->fjss_path).filename();
        if (fs::exists(sheet_path)) {
            check_stylesheet(sheet_path, tally);
        }
    }
}

void check_path(const fs::path& path, Tally& tally) {
    if (fs::is_directory(path)) {
        std::vector<fs::path> entries;
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) entries.push_back(entry.path());
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& entry : entries) {
            auto ext = entry.extension();
            // Widgets check their own sheet; a sheet next to a widget of the
            // same name is reached that way rather than twice.
            if (ext == ".fjwidget") {
                check_widget(entry, tally);
            } else if (ext == ".fjss") {
                auto widget = fs::path(entry).replace_extension(".fjwidget");
                if (!fs::exists(widget)) check_stylesheet(entry, tally);
            }
        }
        return;
    }
    auto ext = path.extension();
    if (ext == ".fjwidget") {
        check_widget(path, tally);
    } else if (ext == ".fjss") {
        check_stylesheet(path, tally);
    } else {
        std::fprintf(stderr, "%s: not a .fjss or .fjwidget file\n", path.string().c_str());
        ++tally.problems;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fjlint <file or directory>...\n");
        return 2;
    }
    log::init();

    Tally tally;
    for (int i = 1; i < argc; ++i) {
        check_path(argv[i], tally);
    }

    int problems = tally.problems + fjlint_logged_problems();
    std::printf("%d file%s checked, %d problem%s\n", tally.files, tally.files == 1 ? "" : "s",
                problems, problems == 1 ? "" : "s");
    log::shutdown();
    return problems == 0 ? 0 : 1;
}
