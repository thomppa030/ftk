#pragma once

#include "core/input_action.hpp"
#include "core/input_mapping_context.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// Parse a .fjinput file into an InputMappingContext struct.
[[nodiscard]] InputMappingContext parse_input_context(const std::string& path);

/// A context read back from its text, as input_context_text() writes it.
/// `where` names it in warnings.
[[nodiscard]] InputMappingContext input_context_from_text(std::string_view text, const std::string& where);

/// Write an InputMappingContext to a .fjinput JSON file.
void write_input_context(const InputMappingContext& ctx, const std::string& path);

/// The .fjinput text write_input_context() writes, without writing it.
[[nodiscard]] std::string input_context_text(const InputMappingContext& ctx);

/// Parse a single .fjaction file into an InputActionDef.
/// Returns a default-constructed def with empty name on failure.
[[nodiscard]] InputActionDef parse_action_file(const std::string& path);

/// Write an InputActionDef to a .fjaction JSON file.
void write_action_file(const InputActionDef& def, const std::string& path);

/// Scan a directory for .fjaction files and return all action definitions.
[[nodiscard]] std::vector<InputActionDef> scan_action_files(const std::string& dir);

} // namespace fjell
