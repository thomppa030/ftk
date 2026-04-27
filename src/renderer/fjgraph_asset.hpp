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
///
/// In v1 the `resources` block is parsed for forward-compat (Step 6 buffer
/// tracking will use it) but per-pass `declare()` still drives actual
/// resource topology in the DAG.

/// One declared resource. v1 stores the raw fields; the engine does not
/// yet allocate from these — `declare()` is still the source of truth.
struct FjGraphResource {
    std::string name;
    std::string format;        // e.g. "RGBA8_SRGB", "D32F"
    std::string size{"viewport"}; // "viewport", "viewport/2", "[w,h]", "$msaa", "match:color"
    int samples{1};
    int mips{1};
    int array_layers{1};
    bool persistent{false};
};

/// One pass entry from the graph's `passes` block.
///
/// `class` is the C++ class name for `PassFactory::make`. `name` defaults
/// to a class-derived key (lowercase + underscores) if omitted, matching
/// the registry's existing string keys. The remaining fields are optional
/// per-pass overrides — when absent, the pass's own `setup()` value wins.
struct FjGraphPass {
    std::string class_name;
    std::string name;          // registry key (defaults from class_name + index)
    PassParams params;

    /// Static enable flag. Default true. Hot-reload re-applies this via
    /// `RenderPipeline::set_pass_enabled` so toggling a pass on or off is
    /// as fast as saving the file.
    bool enabled{true};

    std::optional<std::string> queue;       // "graphics" or "async_compute"
    std::optional<std::string> phase;       // RenderPhase enum name
    std::optional<int> sub_order;
    /// Feature-flag key — `enabled_if`. Reserved for v2 once the engine
    /// has a unified feature registry; today, use the static `enabled`.
    std::optional<std::string> enabled_if;
};

/// Top-level `.fjgraph` document. Parsed once per load; the resolver
/// turns this into a populated `PassRegistry` + `RenderPipeline`.
struct FjGraph {
    std::string name;
    std::vector<FjGraphResource> resources;
    std::vector<FjGraphPass> passes;
    std::string source_path;   // where the graph was loaded from (for hot-reload + errors)
};

} // namespace fjell
