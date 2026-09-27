#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjreflect {

struct FieldInfo {
    std::string name;
    std::string cpp_type;      // "float", "fjell::WeakRef<Node3D>", etc.
    std::string fj_field_type; // "FJELL_FIELD_FLOAT", etc.
    std::string ref_target;    // for WeakRef<X>/Asset<X>: "X", else empty
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
    std::vector<std::string> shader_types; // [[fj::shader_type(water, fog)]] on Asset<Material>

    // Runtime state and Models
    bool is_state{false};      // [[fj::state]], or any member of a reflected struct: bindable, never saved
    bool is_model{false};      // [[fj::model]] on a UI widget: a Model its markup binds
    bool is_prop{false};       // [[fj::prop]] on a UI widget: filled from its widget's prop of that name
    bool is_getter{false};     // a const member function, read through a generated trampoline
    std::string scope;         // qualified name of the declaring class, to resolve struct names in
    std::string struct_type;   // STRUCT: the struct as written; its qualified name once linked
    std::string element_cpp_type; // VECTOR: the element type as written
    std::string element_fj_type;  // VECTOR: the element's kind
    std::string element_struct;   // VECTOR of structs: as written; qualified once linked
};

/// A struct fjreflect reflects because a Model reaches it: every data member,
/// all of them state, and its [[fj::state]] getters.
struct StructInfo {
    std::string name;
    std::string qualified_name;
    std::string source_file;
    unsigned line{0};
    std::vector<FieldInfo> fields;
};

/// A [[fj::pipe]] free function: formats a value for markup, `{value|name:arg}`.
struct PipeInfo {
    std::string name;           // as markup writes it
    std::string qualified_name; // as generated code calls it
    std::string source_file;
    unsigned line{0};
    std::string input_cpp_type; // const and references stripped
    std::string input_fj_type;
    std::vector<std::string> arg_cpp_types; // the literal arguments', likewise
    std::vector<std::string> arg_fj_types;
};

/// A member as declared, kept so the linker can reflect a class a Model reaches.
struct MemberDecl {
    std::string name;
    std::string type;          // a data member's type, or a function's return type
    unsigned line{0};
    bool is_function{false};
    bool is_static{false};
    bool is_const{false};      // a const member function
    bool is_public{false};
    bool is_readable{true};    // one plain declarator: not an array, not several names
    std::vector<std::string> param_types;
    std::vector<std::string> fj_attributes; // names of its [[fj::...]] attributes
};

/// Every class a header declares, components included.
struct ClassDecl {
    std::string name;
    std::string qualified_name;
    std::string source_file;
    unsigned line{0};
    bool is_component{false};  // declared with FJELL_COMPONENT, FJELL_UI_WIDGET or FJELL_NOTIFY
    std::vector<MemberDecl> members;
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

    // How a script component ticks ([[fj::tick(...)]] on the class): its place
    // among the types, lower running earlier, and the seconds between its
    // tick() calls, 0 for every frame.
    bool has_tick{false};
    int tick_order{0};
    float tick_interval{0.0f};
};

struct ParseResult {
    std::vector<ComponentInfo> components;
    std::vector<ClassDecl> classes;
    std::vector<PipeInfo> pipes;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Read a header's classes and collect the components in it: every class
/// that names itself with FJELL_COMPONENT, FJELL_UI_WIDGET, FJELL_UI_COMPONENT
/// or FJELL_NOTIFY, with its [[fj::...]] fields, actions and RPCs. The header
/// is tokenised and read declaration by declaration, so comments, literals
/// and line breaks never change what it means. Anything fjreflect cannot
/// reflect, or an fj attribute where none belongs, is an error with the file
/// and line; code outside reflected classes is skipped.
ParseResult parse_file(const std::string& filepath);

struct LinkResult {
    std::vector<StructInfo> structs;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Resolve what the components of every header refer to: the struct each
/// state field, Model or vector holds is found by name across all headers
/// (from the declaring class's scope outwards) and reflected in full, with
/// every struct it reaches in turn. Fills in the qualified names on the
/// components' fields. Also rejects two pipes of one name.
[[nodiscard]] LinkResult link(std::vector<ComponentInfo>& components, const std::vector<ClassDecl>& classes,
                              const std::vector<PipeInfo>& pipes);


} // namespace fjreflect
