#pragma once

#include "core/input_action.hpp"
#include "core/input_mapping_context.hpp"

#include <string>
#include <vector>

namespace fjell {

/// Parse a .fjinput file into an InputMappingContext struct.
[[nodiscard]] InputMappingContext parse_input_context(const std::string& path);

/// Write an InputMappingContext to a .fjinput JSON file.
void write_input_context(const InputMappingContext& ctx, const std::string& path);

/// Parse a single .fjaction file into an InputActionDef.
/// Returns a default-constructed def with empty name on failure.
[[nodiscard]] InputActionDef parse_action_file(const std::string& path);

/// Scan a directory for .fjaction files and return all action definitions.
[[nodiscard]] std::vector<InputActionDef> scan_action_files(const std::string& dir);

} // namespace fjell
