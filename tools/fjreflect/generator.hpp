#pragma once

#include "parser.hpp"

#include <map>
#include <string>
#include <vector>

namespace fjreflect {

/// Every file fjreflect writes, by name within its output directory.
using GeneratedFiles = std::map<std::string, std::string>;

/// The files for everything reflected: one `<stem>.generated.cpp` per header,
/// holding every component, reflected struct and pipe it declares, and
/// `manifest.json`. Field offsets are left to the compiler that builds the
/// game: the tables use `offsetof`.
[[nodiscard]] GeneratedFiles generate(const std::vector<ComponentInfo>& components,
                                      const std::vector<StructInfo>& structs = {},
                                      const std::vector<PipeInfo>& pipes = {});

/// `register_all.cpp`: the game module's entry points, which hand the engine
/// the reflected structs and pipes and every generated table, then register
/// every component, UI widget and notify, as FJELL_REGISTER() does without
/// codegen.
[[nodiscard]] std::string register_file(const std::vector<ComponentInfo>& components,
                                        const std::vector<StructInfo>& structs = {},
                                        const std::vector<PipeInfo>& pipes = {});

enum class WriteResult { unchanged, written, failed };

/// Write the file only when its content differs from what is there, so an
/// unchanged file keeps its timestamp and is not recompiled.
WriteResult write_if_changed(const std::string& path, const std::string& content);

struct WriteReport {
    std::vector<std::string> written; // files whose content changed
    std::vector<std::string> removed; // outputs of an earlier run nobody wrote this time
};

/// Write the files into the directory, each only when its content changed, so
/// an unchanged file keeps its timestamp and is not recompiled. Removes the
/// outputs of an earlier run that this one did not produce. False, after
/// printing why, when a file could not be written or removed.
bool write_outputs(const std::string& output_dir, const GeneratedFiles& files, WriteReport& report);

} // namespace fjreflect
