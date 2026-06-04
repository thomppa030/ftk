#pragma once

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

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

    // Network replication (null = not replicated). net_serialize appends the
    // component's replicated state to `out`; net_deserialize reads it back from
    // `in`. The netcode treats the bytes as opaque — only the component knows the
    // layout — so it can sit in the per-object state packet alongside replicated
    // script fields and the transform. Used to sync e.g. animator FSM params so
    // remote instances reproduce the same animation locally.
    std::function<void(const void* component, std::vector<uint8_t>& out)> net_serialize{};
    std::function<void(void* component, std::span<const uint8_t> in)> net_deserialize{};
};

} // namespace fjell
