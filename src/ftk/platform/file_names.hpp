#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ftk::platform {

/// A name to give something in a folder, split where a number goes: a
/// file's stem and its extension (`chair`, `.gltf`), a folder's whole name
/// and no extension.
struct NameParts {
    std::string stem;
    std::string extension{};
    /// The folder under the one named that it goes in, as a model's
    /// textures may; empty for that folder itself.
    std::filesystem::path within{};
};

/// The names `names` can take in `folder` without replacing anything there:
/// as they are when none is taken, else each numbered alike before its
/// extension, with the lowest number that leaves every one of them free
/// (`chair_1.gltf`, `chair_1.bin`), so files that name each other stay a
/// set. A name is taken when anything has it where it goes.
[[nodiscard]] std::vector<std::string> free_names(const std::filesystem::path& folder,
                                                  std::span<const NameParts> names);

/// The name `name` can take in `folder`, as free_names() numbers one.
[[nodiscard]] std::string free_name(const std::filesystem::path& folder, const NameParts& name);

} // namespace ftk::platform
