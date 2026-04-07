#pragma once

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

namespace fjell {

class BindlessTextures;
class CommandHistory;
class PipelineCache;
class ProjectManager;
class ResourceManager;
class Scene;
struct Node;
struct Node3D;
class Project;

struct InspectorContext {
    Scene* scene;
    CommandHistory* history;
    ResourceManager* resource_manager;
    PipelineCache* pipeline_cache;
    BindlessTextures* bindless;
    ProjectManager* project_mgr;
};

struct ComponentMeta {
    std::string display_name;
    std::string json_key;
    ImVec4 color;

    std::function<bool(Node3D& node, void* component, const InspectorContext& ctx)> draw;
    std::function<void(Node3D& node, const InspectorContext& ctx)> init;
    std::function<void(const void* component, const Node& node, nlohmann::json& out)> serialize;
    std::function<void(Node& node, const nlohmann::json& j, ResourceManager& res, const Project* proj)> deserialize;
};

} // namespace fjell
