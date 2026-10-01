#include "renderer/resources/fjsl_loader.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace fjell {

namespace {

// A shader's text as the parser sees it: read as text, so line endings are
// the platform's own.
std::optional<std::string> read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Whether a reference and a path from the file watcher name the same file.
// The reference may be relative ("shaders/toon.fjsl") while the watcher
// always reports absolute, so the test is whether one ends with the other,
// anchored to a separator, so "water.fjsl" does not match "deep_water.fjsl".
bool same_shader_file(std::string_view reference, std::string_view changed) {
    if (reference == changed) return true;
    if (reference.size() >= changed.size()) return false;
    if (!changed.ends_with(reference)) return false;
    return changed[changed.size() - reference.size() - 1] == '/';
}

// Every error among `diagnostics`, one per line as file:line:column: message.
std::string error_text(const std::vector<FjslDiagnostic>& diagnostics, const std::string& file) {
    std::string errors;
    for (const auto& d : diagnostics) {
        if (d.severity != FjslSeverity::error) continue;
        if (!errors.empty()) errors += "\n";
        errors += file + ":" + std::to_string(d.line) + ":"
                + std::to_string(d.column) + ": " + d.message;
    }
    return errors;
}

} // namespace

bool FjslProgram::depends_on(std::string_view changed) const {
    return same_shader_file(path, changed);
}

FjslLoader::FjslLoader(FjslLocator locator)
    : locator_{std::move(locator)} {}

FjslProgram FjslLoader::load(const std::string& reference) const {
    FjslProgram program;
    program.path = reference;

    const std::string full_path = locator_ ? locator_(reference) : std::string{};
    if (full_path.empty()) {
        throw std::runtime_error("FJSL file not found: " + reference);
    }
    const std::optional<std::string> source = read_text(full_path);
    if (!source) {
        throw std::runtime_error("Failed to open FJSL file: " + full_path);
    }

    program.metadata = parse_fjsl(*source);
    // The name a compiler error should carry. The parser works on text and
    // does not know where it came from.
    program.metadata.source_path = reference;

    // The parser collects problems rather than throwing on the first, so a
    // shader with two mistakes reports both. Warnings are logged and the
    // build continues; an error means the metadata cannot be built from.
    log_fjsl_diagnostics(program.metadata, reference);
    if (std::string errors = error_text(program.metadata.diagnostics, reference); !errors.empty()) {
        throw std::runtime_error(errors);
    }

    return program;
}

} // namespace fjell
