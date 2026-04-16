#pragma once

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

namespace fjell {

class BindlessTextures;
class CommandHistory;
class ContentBrowser;
class FileBrowser;
class IconCache;
class PipelineCache;
class ProjectManager;
class ResourceManager;
class Scene;
struct Node;
struct Node3D;
class Project;

struct InspectorContext {
    Scene* scene{nullptr};
    CommandHistory* history{nullptr};
    ResourceManager* resource_manager{nullptr};
    PipelineCache* pipeline_cache{nullptr};
    BindlessTextures* bindless{nullptr};
    ProjectManager* project_mgr{nullptr};
    ContentBrowser* content_browser{nullptr};
    FileBrowser* file_browser{nullptr};
    IconCache* icons{nullptr};
};

struct ComponentMeta {
    std::string display_name;
    std::string json_key;
    ImVec4 color;

    std::function<bool(Node3D& node, void* component, const InspectorContext& ctx)> draw;
    std::function<void(Node3D& node, const InspectorContext& ctx)> init;
    std::function<void(const void* component, const Node& node, nlohmann::json& out)> serialize;
    std::function<void(Node& node, const nlohmann::json& j, ResourceManager& res, const Project* proj)> deserialize;

    // Script API dispatch (null = not script-accessible)
    std::string script_type_name;
    void* (*script_get)(Node& node){nullptr};
    void* (*script_add)(Node& node){nullptr};
};

} // namespace fjell
