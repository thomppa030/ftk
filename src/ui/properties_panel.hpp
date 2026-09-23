#pragma once

#include "scene/transform.hpp"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace fjell {

class BindlessTextures;
class CommandHistory;
class EnvironmentSubsystem;
class ContentBrowser;
class FileBrowser;
class IconCache;
class Material;
class PipelineCache;
class ProjectManager;
class ResourceManager;
class Scene;
class ScriptManager;
class TerrainSculptTool;
struct GridSettings;

class PropertiesPanel {
public:
    void init(Scene* scene, uint32_t* selected_id,
              ScriptManager* scripts,
              CommandHistory* history, GridSettings* grid_settings = nullptr,
              PipelineCache* pipeline_cache = nullptr,
              BindlessTextures* bindless = nullptr,
              ResourceManager* resource_manager = nullptr,
              ProjectManager* project_mgr = nullptr,
              ContentBrowser* content_browser = nullptr,
              FileBrowser* file_browser = nullptr,
              IconCache* icons = nullptr,
              TerrainSculptTool* sculpt = nullptr);

    void draw();

    /// The subsystem playing the scene's weather, for the World Environment
    /// inspector's live weather controls. Optional.
    void set_environment(EnvironmentSubsystem* environment) { environment_ = environment; }

private:
    void draw_weather_controls(struct WorldEnvironmentNode& env, const struct InspectorContext& ictx);
    void draw_material_inspector_for(Material* mat);
    void commit_name();

    Scene* scene_{nullptr};
    uint32_t* selected_id_{nullptr};
    EnvironmentSubsystem* environment_{nullptr};
    // The project's .fjweather files for the pick list, rescanned now and then.
    std::vector<std::string> weather_files_;
    double weather_files_scanned_at_{0.0};
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
    TerrainSculptTool* sculpt_{nullptr};

    // The name field's text while it is being typed into, and the node it
    // belongs to. The node is remembered because the click that ends the
    // edit can also change the selection before the edit is committed.
    std::array<char, 128> name_buf_{};
    uint32_t name_node_id_{0};
    bool name_editing_{false};

    // Drag coalescing
    Transform drag_start_transform_;
    std::string drag_start_name_;
};

} // namespace fjell
