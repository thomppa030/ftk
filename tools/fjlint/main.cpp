// fjlint: reports every problem the engine would report when loading
// .fjss stylesheets, .fjwidget files and .fjfont recipes, without starting the engine.
//
//   fjlint <file or directory>...
//   fjlint --stdin-filename <name> -
//
// Directories are searched for *.fjss, *.fjwidget and *.fjfont. A widget's own
// stylesheet is checked along with it. With "-" the stylesheet text is read
// from stdin and reported under the given name, which is how an editor
// checks a buffer while it is being typed, before it is saved. Exit status
// is 1 when anything was reported, so a project's UI can be checked in CI
// or a pre-commit hook.
//
// Reports use the compiler convention "file:line:column: severity: message",
// which editors parse as is. For Neovim with nvim-lint, warnings appear on
// their lines as you type:
//
//   require("lint").linters.fjlint = {
//     cmd = "fjlint", stdin = true, stream = "stdout", ignore_exitcode = true,
//     args = { "--stdin-filename", function() return vim.api.nvim_buf_get_name(0) end, "-" },
//     parser = require("lint.parser").from_errorformat(
//       "%f:%l:%c: %t%*[^:]: %m", { source = "fjlint" }),
//   }
//   require("lint").linters_by_ft.fjss = { "fjlint" }
//   vim.api.nvim_create_autocmd({ "BufWritePost", "TextChanged", "InsertLeave" },
//     { callback = function() require("lint").try_lint() end })
//
// with a filetype rule mapping *.fjss to "fjss". Vim's own :make works with
// `set makeprg=fjlint\ %` and the default errorformat.

#include "ui/game_ui/fjfont.hpp"
#include "ui/game_ui/fjss_loader.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void check_stylesheet_text(const std::string& text, const std::string& name, Tally& tally) {
    // The parser and the loader log each diagnostic; the log sink counts.
    auto sheet = FjssParser::parse(text, name);
    resolve_imports(sheet, fs::path(name).parent_path().string());
    ++tally.files;
}

void check_stylesheet(const fs::path& path, Tally& tally) {
    if (!fs::exists(path)) {
        std::fprintf(stderr, "%s: cannot read\n", path.string().c_str());
        ++tally.problems;
        return;
    }
    // Imports are followed and their problems reported at the @import line
    auto sheet = load_stylesheet(path.string());
    tally.files += static_cast<int>(sheet.files.size());
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

void check_font(const fs::path& path, Tally& tally) {
    ++tally.files;
    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(path, &warnings);
    for (const auto& warning : warnings) FJELL_CORE_WARN("{}", warning);
    if (!recipe) {
        FJELL_CORE_ERROR("{}", recipe.error());
        return;
    }
    auto source = font_source_path(path, *recipe);
    if (!fs::exists(source)) {
        FJELL_CORE_ERROR("{}: source font '{}' does not exist", path.string(), recipe->source);
    }
    if (charset_codepoints(*recipe).empty()) {
        FJELL_CORE_ERROR("{}: the charset is empty", path.string());
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
            } else if (ext == ".fjfont") {
                check_font(entry, tally);
            }
        }
        return;
    }
    auto ext = path.extension();
    if (ext == ".fjwidget") {
        check_widget(path, tally);
    } else if (ext == ".fjss") {
        check_stylesheet(path, tally);
    } else if (ext == ".fjfont") {
        check_font(path, tally);
    } else {
        std::fprintf(stderr, "%s: not a .fjss, .fjwidget or .fjfont file\n", path.string().c_str());
        ++tally.problems;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fjlint <file or directory>...\n"
                             "       fjlint --stdin-filename <name> -\n");
        return 2;
    }
    log::init();

    Tally tally;
    std::string stdin_name = "<stdin>";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stdin-filename") == 0 && i + 1 < argc) {
            stdin_name = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "-") == 0) {
            std::ostringstream text;
            text << std::cin.rdbuf();
            check_stylesheet_text(text.str(), stdin_name, tally);
            continue;
        }
        check_path(argv[i], tally);
    }

    int problems = tally.problems + fjlint_logged_problems();
    std::printf("%d file%s checked, %d problem%s\n", tally.files, tally.files == 1 ? "" : "s",
                problems, problems == 1 ? "" : "s");
    log::shutdown();
    return problems == 0 ? 0 : 1;
}
