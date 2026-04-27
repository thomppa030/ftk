#pragma once

#include "core/result.hpp"
#include "renderer/fjgraph_asset.hpp"

#include <string>
#include <string_view>

namespace fjell {

/// Parse a `.fjgraph` document. Returns a populated `FjGraph` or an error
/// with a `path:line` prefix when the JSON is malformed or violates the
/// schema (unknown pass class, missing required field, illegal value).
class FjGraphParser {
public:
    /// Load and parse a graph file from disk. The returned graph carries
    /// the source path so hot-reload can re-load on inotify events.
    [[nodiscard]] static Result<FjGraph> load(const std::string& path);

    /// Parse from an in-memory buffer. `source_path` is used only for
    /// error messages and is propagated into the returned graph.
    [[nodiscard]] static Result<FjGraph> parse(std::string_view text,
                                               std::string source_path);

private:
    /// Validate a parsed graph against the runtime: every `class` resolves
    /// in `PassFactory`; per-pass `phase`/`queue`/`enabled_if` strings
    /// match known enums or known feature keys. Returns the same graph on
    /// success, otherwise an error.
    [[nodiscard]] static Result<FjGraph> validate(FjGraph graph);
};

} // namespace fjell
