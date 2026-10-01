#pragma once

#include "ftk/base/result.hpp"

#include <glm/vec3.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace fjell {

/// The colours a user has given folders in the content browser.
///
/// Kept in `<project>/.fjell/folder_colors.json`: per project because the
/// folders are, and per user because `.fjell/` is editor state the project's
/// .gitignore leaves out. A colour belongs to one folder; its subfolders do
/// not inherit it.
///
/// Folders are named by absolute path and keyed by their path relative to the
/// project root. Colours are sRGB, as the editor's ImGui colours are.
class FolderColors {
public:
    /// Read the colours for the project at `project_root`, dropping any whose
    /// folder no longer exists. An empty root, or a project without the file,
    /// leaves no colours.
    void load(const std::filesystem::path& project_root);

    /// Write the colours back if anything changed since the last load or save.
    /// Callers save when an edit is finished rather than on every change, so
    /// dragging a colour picker does not rewrite the file each frame.
    Result<> save();

    /// The colour of `folder`, or nothing when it has none.
    [[nodiscard]] std::optional<glm::vec3> find(const std::filesystem::path& folder) const;

    void set(const std::filesystem::path& folder, const glm::vec3& color);
    void clear(const std::filesystem::path& folder);

    /// A folder was renamed or moved: it and every folder beneath it keep
    /// their colours at the new path.
    void moved(const std::filesystem::path& from, const std::filesystem::path& to);

    /// A folder was deleted, and every folder beneath it with it.
    void removed(const std::filesystem::path& folder);

    /// `#rrggbb` for a colour and back; parsing fails on anything else.
    [[nodiscard]] static std::string to_hex(const glm::vec3& color);
    [[nodiscard]] static std::optional<glm::vec3> from_hex(const std::string& hex);

private:
    /// The key of `folder`: its project-relative path with forward slashes,
    /// or empty for the root itself and anything outside the project.
    [[nodiscard]] std::string key(const std::filesystem::path& folder) const;

    std::filesystem::path root_;
    std::filesystem::path file_;
    // Ordered so a folder's subfolders sit right after it and the file
    // diffs cleanly.
    std::map<std::string, glm::vec3> colors_;
    bool dirty_{false};
};

} // namespace fjell
