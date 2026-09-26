#pragma once

#include "renderer/resources/fjsl_parser.hpp" // ShaderType
#include "scripting/fjell/api.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

struct OwnedTypeDesc;

/// A std::vector field's elements, read through the functions generated for
/// its exact type.
struct OwnedVectorDesc {
    FjellFieldType element{FJELL_FIELD_INT};
    const OwnedTypeDesc* element_type{nullptr}; ///< STRUCT elements: their type
    uint32_t element_size{0};
    uint32_t (*size)(const void* vector){nullptr};
    /// Points into the vector; whoever may change the vector may write
    /// through it.
    const void* (*at)(const void* vector, uint32_t index){nullptr};
    void (*insert)(void* vector, uint32_t index){nullptr};
    void (*erase)(void* vector, uint32_t index){nullptr};
    void (*move)(void* vector, uint32_t from, uint32_t to){nullptr};
};

/// The engine's copy of one reflected field, owned so it outlives the game
/// module's tables only as long as the module stays loaded: the function
/// pointers and `type` go with it on a reload.
struct OwnedFieldDesc {
    std::string name;
    FjellFieldType type;
    uint32_t offset;
    uint32_t size;
    std::vector<std::string> enum_labels;
    float min;
    float max;
    float speed;
    std::string ref_type_name;  // for WEAK_REF/REF/SHARED_REF: target type name
    std::string tooltip;        // hover help (empty if none)
    std::string category;       // group header (empty if none)
    bool readonly{false};
    bool replicated{false};     // [[fj::replicated]] — synced over the network
    std::vector<ShaderType> shader_types{}; // Ref<Material>: [[fj::shader_type]] (empty = any)
    uint32_t flags{0};                       // FJELL_FIELD_FLAG_*
    const OwnedTypeDesc* struct_type{nullptr}; // STRUCT: the struct
    OwnedVectorDesc vector{};                  // VECTOR: its elements
    void (*get)(const void* object, void* out){nullptr}; // getters: writes the value

    /// Runtime state rather than an authored value: bindable, never saved.
    [[nodiscard]] bool is_state() const { return (flags & FJELL_FIELD_FLAG_STATE) != 0; }
    /// Read through `get`; `offset` means nothing.
    [[nodiscard]] bool is_getter() const { return (flags & FJELL_FIELD_FLAG_GETTER) != 0; }
    /// A UI widget's Model.
    [[nodiscard]] bool is_model() const { return (flags & FJELL_FIELD_FLAG_MODEL) != 0; }
    /// An authored value the scene file keeps and the inspector edits.
    [[nodiscard]] bool is_property() const {
        return (flags & (FJELL_FIELD_FLAG_STATE | FJELL_FIELD_FLAG_GETTER | FJELL_FIELD_FLAG_MODEL)) == 0 &&
               type != FJELL_FIELD_STRUCT && type != FJELL_FIELD_VECTOR;
    }
};

/// A reflected struct: a widget Model's type, or one nested in it.
struct OwnedTypeDesc {
    std::string name;
    std::string qualified_name;
    uint32_t size{0};
    uint32_t align{0};
    std::vector<OwnedFieldDesc> fields;
    void (*construct)(void* at){nullptr};
    void (*destruct)(void* at){nullptr};

    [[nodiscard]] const OwnedFieldDesc* find_field(std::string_view field) const;
};

/// A UI widget's `[[fj::action]]` method: what markup calls it, what it takes,
/// and how to call it. `invoke` belongs to the game module, like the rest.
struct OwnedUiActionDesc {
    std::string name;
    FjellUiActionParam param{FJELL_UI_ACTION_NONE};
    void (*invoke)(void* self, const void* arg){nullptr};
};

/// A game's formatting pipe, `{value|name:arg}`.
struct OwnedPipeDesc {
    std::string name;
    FjellFieldType input{FJELL_FIELD_INT};
    std::vector<FjellFieldType> arg_types;
    void (*call)(const void* input, const void* const* args, void* out){nullptr};
};

/// The game module's reflected structs and pipes, copied when it registers.
/// Fields that hold a struct, or a vector of them, point at the copies here,
/// so they stay valid until the next load clears them.
class ReflectedTypes {
public:
    void clear();

    /// Copy the module's struct table. Structs may refer to one another in
    /// any order; every reference resolves once all are copied.
    void add_types(const FjellTypeDesc* const* types, uint32_t count);
    void add_pipes(const FjellPipeDesc* pipes, uint32_t count);

    [[nodiscard]] const OwnedTypeDesc* find(std::string_view name) const;
    [[nodiscard]] const OwnedPipeDesc* find_pipe(std::string_view name) const;
    [[nodiscard]] const std::deque<OwnedTypeDesc>& types() const { return types_; }

    /// Copy a generated field table into `out`, resolving struct and vector
    /// element types against the structs added so far. Stops at the first
    /// null name: a count that overruns the table means stale generated code,
    /// and walking past it would crash somewhere unrelated.
    void copy_fields(const FjellFieldDesc* fields, uint32_t count, const char* owner,
                     std::vector<OwnedFieldDesc>& out) const;

private:
    [[nodiscard]] OwnedFieldDesc copy_field(const FjellFieldDesc& fd, const char* owner) const;
    [[nodiscard]] const OwnedTypeDesc* resolve(const FjellTypeDesc* desc) const;

    std::deque<OwnedTypeDesc> types_; // a deque, so fields' pointers into it stay put
    std::unordered_map<const FjellTypeDesc*, const OwnedTypeDesc*> by_desc_;
    std::vector<OwnedPipeDesc> pipes_;
};

} // namespace fjell
