#include "import/import_job.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fjell {

namespace fs = std::filesystem;

ImportJob::~ImportJob() {
    if (state_ == ImportState::running) {
        cancel();
    }
}

std::filesystem::path ImportJob::find_fjimport() {
    auto dir = platform::executable_path().parent_path();
    const auto exe = std::string("fjimport") + platform::EXE_EXT;
    // Same directory as the engine executable (install layout)
    auto candidate = dir / exe;
    if (fs::exists(candidate)) return candidate;
    // Dev build layout: engine at build/src/fjell, fjimport at build/tools/fjimport/fjimport
    candidate = dir.parent_path() / "tools" / "fjimport" / exe;
    if (fs::exists(candidate)) return candidate;
    // MSVC multi-config puts it in a per-config subdirectory.
    for (const char* config : {"Debug", "Release"}) {
        candidate = dir.parent_path() / "tools" / "fjimport" / config / exe;
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

std::filesystem::path ImportJob::write_settings_file(const MeshImportSettings& settings) {
    auto path = fs::temp_directory_path() / "fjimport_settings.json";

    nlohmann::json j;
    j["generate_lods"] = settings.generate_lods;
    j["lod_count"] = settings.lod_count;
    j["generate_sdf"] = settings.generate_sdf;
    j["sdf_resolution"] = settings.sdf_resolution;
    j["import_animations"] = settings.import_animations;
    j["import_textures"] = settings.import_textures;
    j["texture_dir"] = settings.texture_dir;
    j["project_root"] = settings.project_root.string();

    nlohmann::json thresholds = nlohmann::json::array();
    nlohmann::json ratios = nlohmann::json::array();
    for (uint32_t i = 0; i < settings.lod_count; ++i) {
        thresholds.push_back(settings.lod_thresholds[i]);
        ratios.push_back(settings.lod_target_ratios[i]);
    }
    j["lod_thresholds"] = thresholds;
    j["lod_target_ratios"] = ratios;

    std::ofstream f(path);
    f << j.dump(2);
    return path;
}

bool ImportJob::start(const MeshImportSettings& settings) {
    if (state_ == ImportState::running) {
        FJELL_CORE_WARN("Import already running");
        return false;
    }

    auto fjimport_path = find_fjimport();
    if (fjimport_path.empty()) {
        FJELL_CORE_ERROR("Could not find fjimport executable");
        return false;
    }

    // Determine output path
    fs::path source = settings.source_path;
    fs::path output_dir = settings.output_dir;
    if (output_dir.empty()) {
        output_dir = source.parent_path();
    }
    fs::create_directories(output_dir);
    fs::path output_path = output_dir / source.stem();
    output_path.replace_extension(".fjmesh");

    // Write settings
    settings_file_ = write_settings_file(settings);

    // Build args
    std::vector<std::string> args = {
        fjimport_path.string(),
        source.string(),
        "-o", output_path.string(),
        "--json-progress",
        "--settings", settings_file_.string()
    };

    auto result = platform::spawn_with_pipe(args);
    if (!result) {
        FJELL_CORE_ERROR("Failed to spawn fjimport: {}", result.error());
        return false;
    }

    subprocess_ = *result;
    state_ = ImportState::running;
    progress_ = {};
    output_files_.clear();
    line_buffer_.clear();

    FJELL_CORE_INFO("Import started: {}", source.string());
    return true;
}

void ImportJob::poll() {
    if (state_ != ImportState::running) return;

    // Read available lines
    auto lines = platform::read_lines(subprocess_, line_buffer_);
    for (const auto& line : lines) {
        parse_progress_line(line);
    }

    // Check if process exited
    if (platform::poll_exit(subprocess_)) {
        // Drain any remaining lines
        auto remaining = platform::read_lines(subprocess_, line_buffer_);
        for (const auto& line : remaining) {
            parse_progress_line(line);
        }

        bool success = subprocess_.exit_code == 0 && !output_files_.empty();
        finish(success);
    }
}

void ImportJob::cancel() {
    if (state_ != ImportState::running) return;
    platform::close_subprocess(subprocess_);
    state_ = ImportState::idle;
    progress_ = {};
    FJELL_CORE_INFO("Import cancelled");
}

void ImportJob::parse_progress_line(const std::string& line) {
    if (line.empty() || line[0] != '{') return;

    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded()) return;

    progress_.phase = j.value("phase", "");
    progress_.percent = j.value("percent", 0);
    progress_.message = j.value("message", "");

    if (progress_.phase == "done") {
        if (j.contains("output_files")) {
            for (const auto& f : j["output_files"]) {
                output_files_.push_back(f.get<std::string>());
            }
        }
    }
}

void ImportJob::finish(bool success) {
    platform::close_subprocess(subprocess_);

    // Clean up temp settings file
    if (!settings_file_.empty()) {
        std::error_code ec;
        fs::remove(settings_file_, ec);
        settings_file_.clear();
    }

    state_ = success ? ImportState::succeeded : ImportState::failed;
    if (success) {
        FJELL_CORE_INFO("Import complete: {} files", output_files_.size());
    } else {
        FJELL_CORE_ERROR("Import failed (exit code {})", subprocess_.exit_code);
    }

    on_complete.broadcast(success, output_files_);
}

} // namespace fjell
