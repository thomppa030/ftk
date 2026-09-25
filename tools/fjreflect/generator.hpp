#pragma once

#include "parser.hpp"

#include <map>
#include <string>
#include <vector>

namespace fjreflect {

/// Every file fjreflect writes, by name within its output directory.
using GeneratedFiles = std::map<std::string, std::string>;

/// The files for these components: one `<stem>.generated.cpp` and one
/// `<stem>.verify.cpp` per header, holding every component it declares, and
/// `manifest.json`. Offsets must already be filled in.
[[nodiscard]] GeneratedFiles generate(const std::vector<ComponentInfo>& components);

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
