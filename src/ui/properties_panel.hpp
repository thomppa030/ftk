#pragma once

#include "scene/transform.hpp"
#include "scene/component_storage.hpp"
#include "ui/edit_tracker.hpp"

#include <nlohmann/json.hpp>
#include "ui/kit/text_field.hpp"

#include <cstdint>
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
struct CameraNode;
struct DirectionalLightNode;
struct FogVolumeNode;
struct GridSettings;
struct InspectorContext;
struct Node;
struct Node3D;
struct PointLightNode;
struct SpotLightNode;
struct VolumetricFogNode;
struct WorldEnvironmentNode;

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
    // The panel's parts, top to bottom.
    void draw_name(const Node& selected);
    void draw_transform(Node3D& node);
    void draw_node_sections(Node3D& node, const InspectorContext& ictx);
    void draw_components(Node3D& node, const InspectorContext& ictx);
    void draw_scripts(Node3D& node);

    // A node type's own section (properties_node_sections.cpp). Each
    // returns whether a setting changed; the caller turns that into dirty
    // and undo.
    bool draw_camera(CameraNode& camera);
    bool draw_directional_light(DirectionalLightNode& light);
    bool draw_point_light(PointLightNode& light);
    bool draw_spot_light(SpotLightNode& light);
    bool draw_volumetric_fog(VolumetricFogNode& fog, const InspectorContext& ictx);
    bool draw_fog_volume(FogVolumeNode& volume, const InspectorContext& ictx);
    bool draw_world_environment(WorldEnvironmentNode& env, const InspectorContext& ictx);
    bool draw_weather_controls(WorldEnvironmentNode& env, const InspectorContext& ictx);
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

    // The node name field and the node its current edit belongs to.
    ui::TextField name_field_;
    uint32_t name_node_id_{0};

    // Undo steps for edits made in the component blocks and in the node's
    // own section.
    EditTracker<ComponentStorage> component_edits_;
    EditTracker<nlohmann::json> node_edits_;

    // The transform as it was before the edit under way, for its undo step.
    Transform drag_start_transform_;
    std::string drag_start_name_;
    bool transform_editing_{false};
};

} // namespace fjell
