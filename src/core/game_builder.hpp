#pragma once

#include "core/result.hpp"

#include <filesystem>

namespace fjell {

class Project;

class GameBuilder {
public:
    static Result<> build(const Project& project,
                          const std::filesystem::path& output_dir);
};

} // namespace fjell
