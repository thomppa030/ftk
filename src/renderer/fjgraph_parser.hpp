#pragma once

#include "ftk/base/result.hpp"
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
    [[nodiscard]] static ftk::Result<FjGraph> load(const std::string& path);

    /// Parse from an in-memory buffer. `source_path` is used only for
    /// error messages and is propagated into the returned graph.
    [[nodiscard]] static ftk::Result<FjGraph> parse(std::string_view text,
                                               std::string source_path);

private:
    /// Validate a parsed graph against the runtime: every `class` resolves
    /// in `PassFactory` and every pass name is unique. Returns the same graph
    /// on success, otherwise an error.
    [[nodiscard]] static ftk::Result<FjGraph> validate(FjGraph graph);
};

} // namespace fjell
