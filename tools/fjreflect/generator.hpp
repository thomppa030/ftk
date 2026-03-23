#pragma once

#include "parser.hpp"

#include <string>
#include <vector>

namespace fjreflect {

struct GenerateOptions {
    std::string output_dir;
};

/// Generate .generated.cpp, .verify.cpp, and manifest.json from parsed components.
void generate(const std::vector<ComponentInfo>& components,
              const GenerateOptions& opts);

} // namespace fjreflect
