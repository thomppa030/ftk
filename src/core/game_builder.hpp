#pragma once

#include "ftk/base/result.hpp"

#include <filesystem>

namespace fjell {

class Project;

class GameBuilder {
public:
    static ftk::Result<> build(const Project& project,
                          const std::filesystem::path& output_dir);
};

} // namespace fjell
