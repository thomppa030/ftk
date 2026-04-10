#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

struct MeshImportSettings {
    std::filesystem::path source_path;
    std::filesystem::path output_dir;

    bool generate_lods{true};
    uint32_t lod_count{3};
    float lod_thresholds[8]{1.0f, 0.3f, 0.1f};
    float lod_target_ratios[8]{1.0f, 0.5f, 0.25f};

    bool generate_sdf{true};
    uint32_t sdf_resolution{32};

    bool import_animations{true};
};

struct ImportProgress {
    std::string phase;    // "loading", "mesh", "lod", "sdf", "write", "done", "error"
    int percent{0};       // 0-100
    std::string message;
    std::vector<std::string> output_files; // populated on "done"
};

enum class ImportState {
    idle,
    configuring,  // dialog open, user editing settings
    running,      // subprocess executing
    succeeded,
    failed
};

} // namespace fjell
