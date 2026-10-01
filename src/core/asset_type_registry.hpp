#pragma once

#include "ftk/base/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class ResourceManager;

using AssetTypeId = uint8_t;

class IconCache;
class Project;

struct AssetTypeEntry {
    AssetTypeId    type_id{0};       // auto-assigned by registry
    std::string    type_name;        // "Vfx", "Audio", etc.: what scripts write in Asset<T>
    std::string    extension;        // ".fjvfx", ".fjaudio", etc.
    std::string    label;            // "VFX Effect", "Audio Clip", etc.

    void*            (*resolve)(ResourceManager& rm, uint32_t handle_id){nullptr};
    std::string      (*path_of)(const ResourceManager& rm, uint32_t handle_id){nullptr};
    Result<uint32_t> (*load)(ResourceManager& rm, const std::string& path){nullptr};
    /// The handle kept for `path`, whether it loaded or failed to load; 0
    /// when nothing was ever loaded under it. What a field that names a file
    /// which failed to load holds, so the file is still known.
    uint32_t         (*find)(const ResourceManager& rm, const std::string& path){nullptr};

    /// Optional preview callback for the inspector widget.
    /// Draws a type-specific preview (thumbnail, metadata, etc.) at the given width.
    /// path is project-relative, project may be null.
    void (*draw_preview)(const std::string& path, ResourceManager& rm,
                         IconCache* icons, const Project* project,
                         float width){nullptr};
};

class AssetTypeRegistry {
public:
    static AssetTypeRegistry& instance();

    void register_type(AssetTypeEntry entry);

    [[nodiscard]] const AssetTypeEntry* find_by_name(std::string_view name) const;
    [[nodiscard]] const AssetTypeEntry* find_by_id(AssetTypeId id) const;

    template <typename Fn>
    void for_each(const Fn& fn) const {
        for (const auto& e : entries_) {
            fn(e);
        }
    }

private:
    std::vector<AssetTypeEntry> entries_;
    AssetTypeId next_id_{1}; // 0 = invalid/unset
};

} // namespace fjell
