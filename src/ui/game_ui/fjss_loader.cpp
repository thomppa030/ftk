#include "ui/game_ui/fjss_loader.hpp"
#include "ftk/base/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fjell {

namespace {

namespace fs = std::filesystem;

std::string normalized(const std::string& path) {
    return fs::path(path).lexically_normal().string();
}

void add_file(Stylesheet& sheet, const std::string& path) {
    if (std::find(sheet.files.begin(), sheet.files.end(), path) == sheet.files.end()) {
        sheet.files.push_back(path);
    }
}

void report(Stylesheet& sheet, const std::string& source, uint32_t line, uint32_t column,
            std::string message) {
    // Loader problems are logged the way the parser logs its own, so the
    // console and fjlint show them in the same place and format.
    FTK_CORE_ERROR("{}:{}:{}: error: {}", source, line, column, message);
    sheet.diagnostics.push_back({source, line, column, FjssSeverity::error, std::move(message)});
}

void report(Stylesheet& sheet, const StyleImport& import, std::string message) {
    report(sheet, import.source, import.line, import.column, std::move(message));
}

// A face's recipe is named relative to the file that declares it, like an
// import. Resolved once; a face merged from an imported sheet keeps the
// path its own file resolved.
void resolve_font_faces(Stylesheet& sheet, const std::string& base_dir) {
    for (auto& face : sheet.font_faces) {
        if (!face.resolved_src.empty()) continue;
        auto path = normalized((fs::path(base_dir) / face.src).string());
        if (!fs::exists(path)) {
            report(sheet, face.source, face.line, face.column,
                   "@font-face src '" + face.src + "' does not exist");
        }
        face.resolved_src = path;
    }
}

// Files currently being resolved, outermost first, to catch a cycle.
void resolve(Stylesheet& sheet, const std::string& base_dir, std::vector<std::string>& chain);

Stylesheet load(const std::string& path, std::vector<std::string>& chain) {
    std::ifstream file(path);
    Stylesheet sheet;
    if (!file) return sheet;
    std::ostringstream text;
    text << file.rdbuf();
    sheet = FjssParser::parse(text.str(), path);
    add_file(sheet, path);
    chain.push_back(normalized(path));
    resolve(sheet, fs::path(path).parent_path().string(), chain);
    chain.pop_back();
    return sheet;
}

void resolve(Stylesheet& sheet, const std::string& base_dir, std::vector<std::string>& chain) {
    resolve_font_faces(sheet, base_dir);

    // Each import's rules go where the import was written; every splice
    // shifts the indices of the imports after it.
    size_t inserted = 0;
    std::vector<StyleImport> nested;
    for (const auto& import : sheet.imports) {
        auto path = (fs::path(base_dir) / import.path).string();
        auto key = normalized(path);
        if (std::find(chain.begin(), chain.end(), key) != chain.end()) {
            report(sheet, import, "'" + import.path + "' imports itself, directly or through another file");
            continue;
        }
        if (!fs::exists(path)) {
            report(sheet, import, "cannot read '" + import.path + "'");
            continue;
        }
        Stylesheet imported = load(path, chain);
        auto at = sheet.rules.begin() + static_cast<std::ptrdiff_t>(import.rule_index + inserted);
        sheet.rules.insert(at, std::make_move_iterator(imported.rules.begin()),
                           std::make_move_iterator(imported.rules.end()));
        inserted += imported.rules.size();
        nested.insert(nested.end(), imported.imports.begin(), imported.imports.end());
        sheet.font_faces.insert(sheet.font_faces.end(),
                                std::make_move_iterator(imported.font_faces.begin()),
                                std::make_move_iterator(imported.font_faces.end()));
        sheet.diagnostics.insert(sheet.diagnostics.end(), imported.diagnostics.begin(),
                                 imported.diagnostics.end());
        for (const auto& f : imported.files) add_file(sheet, f);
    }
    // Imports of imported files stay in the sheet so each file can still be
    // written back with its own @import lines.
    sheet.imports.insert(sheet.imports.end(), nested.begin(), nested.end());
}

} // namespace

Stylesheet load_stylesheet(const std::string& path) {
    std::vector<std::string> chain;
    return load(path, chain);
}

void resolve_imports(Stylesheet& sheet, const std::string& base_dir) {
    std::vector<std::string> chain;
    resolve(sheet, base_dir, chain);
}

} // namespace fjell
