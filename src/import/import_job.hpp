#pragma once

#include "import/import_types.hpp"
#include "ftk/base/delegate.hpp"
#include "ftk/platform/platform.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

/// Manages a single fjimport subprocess. Spawns the process, reads JSON
/// progress from its stdout pipe, and fires on_complete when done.
class ImportJob {
public:
    ImportJob() = default;
    ~ImportJob();

    ImportJob(const ImportJob&) = delete;
    ImportJob& operator=(const ImportJob&) = delete;

    /// Start an import. Writes a temp settings JSON and spawns fjimport.
    bool start(const MeshImportSettings& settings);

    /// Poll for progress. Call once per frame. Parses JSON lines from the pipe.
    void poll();

    /// Kill a running import.
    void cancel();

    [[nodiscard]] ImportState state() const { return state_; }
    [[nodiscard]] const ImportProgress& progress() const { return progress_; }
    [[nodiscard]] const std::vector<std::string>& output_files() const { return output_files_; }

    /// Fired when import completes (success or failure).
    Delegate<void(bool success, const std::vector<std::string>& output_files)> on_complete;

    /// Where the fjimport executable is: beside the engine when installed,
    /// in the build tree during development; empty when it isn't found.
    [[nodiscard]] static std::filesystem::path find_fjimport();

private:
    [[nodiscard]] std::filesystem::path write_settings_file(const MeshImportSettings& settings);
    void parse_progress_line(const std::string& line);
    void finish(bool success);

    ImportState state_{ImportState::idle};
    ImportProgress progress_;
    platform::SubprocessHandle subprocess_;
    std::string line_buffer_;
    std::filesystem::path settings_file_;
    std::vector<std::string> output_files_;
};

} // namespace fjell
