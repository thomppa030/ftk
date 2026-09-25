#pragma once

#include "renderer/pass_factory.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fjell {

/// Parsed `.fjgraph` representation.
///
/// `.fjgraph` defines render pipeline **topology** (which passes run, in
/// what order, with what overrides). Pass **behavior** (shaders, draws,
/// dispatches, descriptor wiring) stays in C++ in the `RenderPass`
/// subclass. The `class` field is the binding key into `PassFactory`.
/// What a pass reads and writes comes from its `declare()`, and where it
/// runs from its `setup()`.

/// One pass entry from the graph's `passes` block.
///
/// `class` is the C++ class name for `PassFactory::make`. `name` defaults
/// to a class-derived key (lowercase + underscores) if omitted, matching
/// the registry's existing string keys. `params` reach the pass's
/// constructor.
struct FjGraphPass {
    std::string class_name;
    std::string name;          // registry key (defaults from class_name + index)
    PassParams params;

    /// Static enable flag. Default true. Hot-reload re-applies this via
    /// `RenderPipeline::set_pass_enabled` so toggling a pass on or off is
    /// as fast as saving the file.
    bool enabled{true};

    /// A device feature the pass needs, `mesh_shaders` today. Without it
    /// the pass is not constructed at all.
    std::optional<std::string> enabled_if;
};

/// Top-level `.fjgraph` document. Parsed once per load; the resolver
/// turns this into a populated `PassRegistry` + `RenderPipeline`.
struct FjGraph {
    std::string name;
    std::vector<FjGraphPass> passes;
    std::string source_path;   // where the graph was loaded from (for hot-reload + errors)
};

} // namespace fjell
