#pragma once

#include "ui/game_ui/fjss_parser.hpp"

#include <string>

namespace fjell {

/// Reads a stylesheet from disk and splices in every `@import` it names,
/// recursively, at the point where the import is written. Rules keep the
/// file they came from as their source, so a save of the importing file
/// writes its own rules and `@import` lines and nothing from the imported
/// ones. The result lists every file it was assembled from.
///
/// A file that cannot be read, or one that imports itself through any
/// chain, is reported as a diagnostic at the `@import` line and skipped;
/// the rest of the sheet still loads.
[[nodiscard]] Stylesheet load_stylesheet(const std::string& path);

/// Resolves the imports of a sheet parsed from text that did not come from
/// a file, relative to `base_dir`.
void resolve_imports(Stylesheet& sheet, const std::string& base_dir);

} // namespace fjell
