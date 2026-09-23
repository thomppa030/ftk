#pragma once

#include <cstdint>

namespace fjell {

class PipelineCache;
class Project;

/// When the shader `pipeline_id` failed to compile, the callout saying so
/// where its values are edited: its first error and line, what draws in the
/// meantime, and a button that opens the file. Returns whether it failed.
bool shader_error_callout(const PipelineCache& cache, uint32_t pipeline_id, const Project* project);

} // namespace fjell
