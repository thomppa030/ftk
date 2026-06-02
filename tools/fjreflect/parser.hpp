#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjreflect {

struct FieldInfo {
    std::string name;
    std::string cpp_type;      // "float", "fjell::WeakRef<Node3D>", etc.
    std::string fj_field_type; // "FJELL_FIELD_FLOAT", etc.
    std::string ref_target;    // for WeakRef<X>/Ref<X>: "X", else empty
    uint32_t offset{0};        // filled later by offset query
    uint32_t size{0};          // filled later by offset query
    unsigned line{0};          // source line number

    // Hints
    float hint_min{0.0f};
    float hint_max{0.0f};
    float hint_speed{0.0f};
    bool has_range{false};
    bool is_color{false};      // [[fj::color]] — vec3/vec4 → COLOR3/COLOR4
    bool is_readonly{false};   // [[fj::readonly]] — disabled in inspector
    bool is_replicated{false}; // [[fj::replicated]] — synced over the network
    std::string tooltip;       // [[fj::tooltip("...")]]
    std::string category;      // [[fj::category("...")]]
    std::vector<std::string> enum_labels; // [[fj::enum(A, B, C)]]

    // UI bind flags
    bool is_bind{false};       // [[fj::bind]] — auto-sync to widget context
    bool is_bind_list{false};  // [[fj::bind_list]] — sync vector<T> to foreach list
    std::string bind_key;      // explicit key override (empty = use field name)
};

enum class ActionParamType : uint8_t {
    none,          // void()
    int_param,     // void(int)
    float_param,   // void(float)
    vec2_param,    // void(glm::vec2)
};

enum class ActionTriggerMode : uint8_t {
    triggered,     // default: fires when action becomes active
    pressed,       // button: fires on press
    released,      // button: fires on release
    completed,     // fires when action becomes inactive
};

struct ActionInfo {
    std::string name;          // action name (default = method name)
    std::string method_name;   // C++ method name
    ActionParamType param{ActionParamType::none};
    ActionTriggerMode trigger{ActionTriggerMode::triggered};
    bool has_int_param{false}; // backward compat (= param == int_param)
};

enum class RpcTarget : uint8_t {
    server,   // client → server
    clients,  // server → all clients
    all,      // sender → all peers
};

struct RpcParam {
    std::string cpp_type;  // e.g. "glm::vec3", "int", "float"
    std::string name;      // parameter name (for the generated wrapper signature)
};

struct RpcInfo {
    std::string method_name;          // e.g. "request_fire" (the wrapper)
    RpcTarget target{RpcTarget::server};
    std::vector<RpcParam> params;     // fixed-size POD params, in order
};

struct ComponentInfo {
    std::string class_name;       // bare name: "CoinCollectible"
    std::string qualified_name;   // namespace-qualified: "fjell::CoinCollectible" (or bare if global)
    std::string source_file;
    std::vector<FieldInfo> fields;

    // UI component specifics
    bool is_ui_component{false};
    std::string widget_tag;       // from FJELL_UI_COMPONENT(T, "tag")
    std::vector<ActionInfo> actions;

    // Networked RPC methods ([[fj::rpc(target)]])
    std::vector<RpcInfo> rpcs;

    // Notify type (FJELL_NOTIFY)
    bool is_notify{false};
};

struct ParseResult {
    std::vector<ComponentInfo> components;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Text-based parse: find FJELL_COMPONENT / FJELL_UI_COMPONENT classes
/// and [[fj::property]] / [[fj::bind]] / [[fj::action]] members.
ParseResult parse_file(const std::string& filepath);

/// Compile and run a tiny program to query offsetof/sizeof for all fields.
/// Fills in offset and size on each FieldInfo. Returns false on failure.
bool query_offsets(std::vector<ComponentInfo>& components,
                   const std::string& source_file,
                   const std::vector<std::string>& include_dirs,
                   const std::string& compiler);

} // namespace fjreflect
