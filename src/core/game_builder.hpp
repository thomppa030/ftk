#pragma once

#include <filesystem>

namespace fjell {

class Project;

class GameBuilder {
public:
    static bool build(const Project& project,
                      const std::filesystem::path& output_dir);
};

} // namespace fjell
