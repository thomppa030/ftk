#pragma once

#include "scene/transform.hpp"

#include <cstdint>
#include <string>

namespace fjell {

class BindlessTextures;
class CommandHistory;
class ContentBrowser;
class FileBrowser;
class IconCache;
class Material;
class PipelineCache;
class ProjectManager;
class ResourceManager;
class Scene;
class ScriptManager;
struct DirectionalLight;
struct GridSettings;

class PropertiesPanel {
public:
    void init(Scene* scene, uint32_t* selected_id, int* gizmo_op,
              DirectionalLight* light, ScriptManager* scripts,
              CommandHistory* history, GridSettings* grid_settings = nullptr,
              PipelineCache* pipeline_cache = nullptr,
              BindlessTextures* bindless = nullptr,
              ResourceManager* resource_manager = nullptr,
              ProjectManager* project_mgr = nullptr,
              ContentBrowser* content_browser = nullptr,
              FileBrowser* file_browser = nullptr,
              IconCache* icons = nullptr);

    void draw();

private:
    void draw_material_inspector(struct Node3D& obj);
    void draw_material_inspector_for(Material* mat);

    Scene* scene_{nullptr};
    uint32_t* selected_id_{nullptr};
    int* gizmo_op_{nullptr};
    DirectionalLight* light_{nullptr};
    ScriptManager* scripts_{nullptr};
    CommandHistory* history_{nullptr};
    GridSettings* grid_settings_{nullptr};
    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};
    ResourceManager* resource_manager_{nullptr};
    ProjectManager* project_mgr_{nullptr};
    ContentBrowser* content_browser_{nullptr};
    FileBrowser* file_browser_{nullptr};
    IconCache* icons_{nullptr};

    // Drag coalescing
    Transform drag_start_transform_;
    std::string drag_start_name_;
};

} // namespace fjell
