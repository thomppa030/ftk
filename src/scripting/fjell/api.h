// fjell_api.h — Fjell Engine field reflection types
// Shared by the engine and game .so for inspector/serialization.

#ifndef FJELL_SCRIPTING_API_H
#define FJELL_SCRIPTING_API_H

#include <stdint.h>
#include <stddef.h>

// FJELL_SDK_EXPORT marks symbols exported from the engine exe for script DLLs.
// When building the engine (FJELL_ENGINE_BUILD), symbols are exported.
// When building game scripts (DLL), symbols are imported.
#ifdef _WIN32
    #ifdef FJELL_ENGINE_BUILD
        #define FJELL_SDK_EXPORT __declspec(dllexport)
    #else
        #define FJELL_SDK_EXPORT __declspec(dllimport)
    #endif
    // FJELL_GAME_EXPORT marks symbols exported FROM the game DLL to the engine.
    #define FJELL_GAME_EXPORT __declspec(dllexport)
#else
    #define FJELL_SDK_EXPORT
    #define FJELL_GAME_EXPORT
#endif

// FJELL_PRINTF_FMT marks a printf-style function so the compiler type-checks
// the format string against its arguments at the call site. fmt_idx and
// va_idx are 1-based argument positions; for a static member function the
// implicit `this` is absent, so they count from the first declared parameter.
// MSVC has no equivalent attribute — it expands to nothing there.
#if defined(__GNUC__) || defined(__clang__)
    #define FJELL_PRINTF_FMT(fmt_idx, va_idx) \
        __attribute__((format(printf, fmt_idx, va_idx)))
#else
    #define FJELL_PRINTF_FMT(fmt_idx, va_idx)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ── API version ─────────────────────────────────────────────────────────
// Bump this when changing any SDK struct layout, vtable shape, or ABI contract.
// Engine checks this at .so load time and rejects mismatches.
#define FJELL_API_VERSION 29

// ── Engine component layout check ───────────────────────────────────────
// The size of an engine component struct as the game module's copy of
// engine_components.hpp defines it. The module publishes one entry per
// mirrored struct; the engine compares each against its own definition when
// loading the module and refuses a stale build by name, rather than reading
// and writing fields at the wrong offsets.
typedef struct FjellComponentLayout {
    const char* type_name;
    uint32_t size;
} FjellComponentLayout;

// ── Field types for inspector / serialization reflection ────────────────

typedef enum FjellFieldType {
    FJELL_FIELD_FLOAT = 0,
    FJELL_FIELD_INT,
    FJELL_FIELD_BOOL,
    FJELL_FIELD_VEC2,
    FJELL_FIELD_VEC3,
    FJELL_FIELD_VEC4,
    FJELL_FIELD_COLOR3,
    FJELL_FIELD_COLOR4,
    FJELL_FIELD_STRING,        // fixed char[N] buffer; `size` is its capacity
    FJELL_FIELD_ENUM,
    FJELL_FIELD_WEAK_REF,
    FJELL_FIELD_REF,
    FJELL_FIELD_WIDGET_PTR,    // [[fj::bind]] WidgetType* — resolved by UiSubsystem
    // New enumerators go at the end: inserting one mid-enum renumbers the rest
    // and silently misreads every already-compiled game module.
    FJELL_FIELD_STD_STRING,    // std::string, owned by the component
    FJELL_FIELD_STRUCT,        // a reflected struct held by value; `type_desc` describes it
    FJELL_FIELD_VECTOR         // a std::vector; `vector` describes its elements and reads it
} FjellFieldType;

// What a field is for, beyond its type. A field with none of these is a
// property: authored, saved with the scene, edited in the inspector.
enum {
    // Runtime state the UI may read: shown in the inspector during play and
    // never saved. The members of a reflected struct are all state.
    FJELL_FIELD_FLAG_STATE = 1u << 0,
    // A const member function, not a member: its value is read through `get`,
    // and `offset` means nothing. Always also state.
    FJELL_FIELD_FLAG_GETTER = 1u << 1,
    // A UI widget's Model: a struct the widget's markup binds by the field's name.
    FJELL_FIELD_FLAG_MODEL = 1u << 2
};

struct FjellTypeDesc;
struct FjellVectorDesc;

typedef struct FjellFieldDesc {
    const char*        name;
    FjellFieldType     type;
    uint32_t           offset;       // offsetof() from data start
    uint32_t           size;         // STRING: buffer capacity; ENUM: label count
    const char* const* enum_labels;  // ENUM only (null otherwise)
    float              min;
    float              max;
    float              speed;        // drag speed (0 = default 0.1)
    const char*        ref_type_name; // REF/WEAK_REF/SHARED_REF: target type name (null otherwise)
    const char*        tooltip;      // hover help in inspector (null if none)
    const char*        category;     // group header in inspector (null if none)
    uint8_t            readonly;     // 1 = inspector shows but disables editing
    uint8_t            replicated;   // 1 = [[fj::replicated]], synced over the network
    const char*        shader_types; // Ref<Material>: [[fj::shader_type]] names, comma-separated (null = any)
    uint32_t           flags;        // FJELL_FIELD_FLAG_* (0 = a property)
    const struct FjellTypeDesc*   type_desc; // STRUCT: the struct (null otherwise)
    const struct FjellVectorDesc* vector;    // VECTOR: its elements (null otherwise)
    // GETTER: writes the value into `out`, which holds the field's type
    // (std::string for STD_STRING). Null otherwise.
    void (*get)(const void* object, void* out);
} FjellFieldDesc;

// A struct fjreflect reflects so the UI can read it: the type of a widget's
// Model, of a field inside one, or of a vector's elements.
typedef struct FjellTypeDesc {
    const char*           name;           // as spelled in the header, e.g. "SlotModel"
    const char*           qualified_name; // with its namespaces, e.g. "game::SlotModel"
    uint32_t              size;
    uint32_t              align;
    const FjellFieldDesc* fields;         // every data member, each flagged STATE
    uint32_t              field_count;
    void (*construct)(void* at);          // default-constructs one at `at` (size/align bytes)
    void (*destruct)(void* at);
} FjellTypeDesc;

// A std::vector's elements, read through functions generated for its exact
// type: the engine never assumes a vector's layout.
typedef struct FjellVectorDesc {
    FjellFieldType       element;         // the elements' kind
    const FjellTypeDesc* element_type;    // STRUCT elements: their type (null otherwise)
    uint32_t             element_size;
    uint32_t    (*size)(const void* vector);
    const void* (*at)(const void* vector, uint32_t index);
} FjellVectorDesc;

// A game's formatting pipe, [[fj::pipe]]: `{value|name:arg}` in markup.
typedef struct FjellPipeDesc {
    const char*           name;
    FjellFieldType        input;          // the value's kind
    const FjellFieldType* arg_types;      // the literal arguments' kinds, in order
    uint32_t              arg_count;
    // Formats *input with the arguments into `out`, a std::string.
    void (*call)(const void* input, const void* const* args, void* out);
} FjellPipeDesc;

// ── Action types for [[fj::action]] input binding ────────────────────

typedef enum FjellActionType {
    FJELL_ACTION_BUTTON = 0,
    FJELL_ACTION_AXIS1D,
    FJELL_ACTION_AXIS2D,
} FjellActionType;

typedef enum FjellActionTrigger {
    FJELL_TRIGGER_TRIGGERED = 0,  // default: fires once when action becomes active
    FJELL_TRIGGER_PRESSED,        // button: fires once on press
    FJELL_TRIGGER_RELEASED,       // button: fires once on release
    FJELL_TRIGGER_COMPLETED,      // fires once when action becomes inactive
} FjellActionTrigger;

typedef struct FjellActionDesc {
    const char*        name;        // action method name
    FjellActionType    type;        // button, axis1d, axis2d
    FjellActionTrigger trigger;     // when to fire
    void*              trampoline;  // function pointer (cast to appropriate signature)
} FjellActionDesc;

// ── RPC descriptors for [[fj::rpc(target)]] networked methods ──────────

typedef enum FjellRpcTarget {
    FJELL_RPC_SERVER  = 0,  // client → server
    FJELL_RPC_CLIENTS = 1,  // server → all clients
    FJELL_RPC_ALL     = 2,  // sender → all peers (incl. self)
} FjellRpcTarget;

typedef struct FjellRpcDesc {
    const char*    name;          // RPC method name (the wrapper, not the _impl)
    FjellRpcTarget target;        // where the handler runs
    uint32_t       payload_size;  // total bytes of POD params (fixed per RPC)
    // Deserialize `in` (payload_size bytes) and call self->name_impl(args...).
    void (*invoke)(void* self, const uint8_t* in, uint32_t size);
} FjellRpcDesc;

// ── UI widget action descriptors (for on:click validation) ─────────────

typedef enum FjellUiActionParam {
    FJELL_UI_ACTION_NONE = 0,
    FJELL_UI_ACTION_INT,
    FJELL_UI_ACTION_FLOAT,
    FJELL_UI_ACTION_VEC2,
} FjellUiActionParam;

typedef struct FjellUiActionDesc {
    const char*        name;        // action name
    FjellUiActionParam param_type;  // parameter type
} FjellUiActionDesc;

// ── Key codes (match GLFW values) ────────────────────────────────────

#define FJELL_KEY_SPACE          32
#define FJELL_KEY_APOSTROPHE     39
#define FJELL_KEY_COMMA          44
#define FJELL_KEY_MINUS          45
#define FJELL_KEY_PERIOD         46
#define FJELL_KEY_SLASH          47
#define FJELL_KEY_0              48
#define FJELL_KEY_1              49
#define FJELL_KEY_2              50
#define FJELL_KEY_3              51
#define FJELL_KEY_4              52
#define FJELL_KEY_5              53
#define FJELL_KEY_6              54
#define FJELL_KEY_7              55
#define FJELL_KEY_8              56
#define FJELL_KEY_9              57
#define FJELL_KEY_A              65
#define FJELL_KEY_B              66
#define FJELL_KEY_C              67
#define FJELL_KEY_D              68
#define FJELL_KEY_E              69
#define FJELL_KEY_F              70
#define FJELL_KEY_G              71
#define FJELL_KEY_H              72
#define FJELL_KEY_I              73
#define FJELL_KEY_J              74
#define FJELL_KEY_K              75
#define FJELL_KEY_L              76
#define FJELL_KEY_M              77
#define FJELL_KEY_N              78
#define FJELL_KEY_O              79
#define FJELL_KEY_P              80
#define FJELL_KEY_Q              81
#define FJELL_KEY_R              82
#define FJELL_KEY_S              83
#define FJELL_KEY_T              84
#define FJELL_KEY_U              85
#define FJELL_KEY_V              86
#define FJELL_KEY_W              87
#define FJELL_KEY_X              88
#define FJELL_KEY_Y              89
#define FJELL_KEY_Z              90
#define FJELL_KEY_ESCAPE         256
#define FJELL_KEY_ENTER          257
#define FJELL_KEY_TAB            258
#define FJELL_KEY_BACKSPACE      259
#define FJELL_KEY_RIGHT          262
#define FJELL_KEY_LEFT           263
#define FJELL_KEY_DOWN           264
#define FJELL_KEY_UP             265
#define FJELL_KEY_LEFT_SHIFT     340
#define FJELL_KEY_LEFT_CONTROL   341
#define FJELL_KEY_LEFT_ALT       342
#define FJELL_KEY_RIGHT_SHIFT    344
#define FJELL_KEY_RIGHT_CONTROL  345
#define FJELL_KEY_RIGHT_ALT      346

#define FJELL_MOUSE_LEFT         0
#define FJELL_MOUSE_RIGHT        1
#define FJELL_MOUSE_MIDDLE       2

#ifdef __cplusplus
}

// ── C++ input trigger enum for [[fj::action(EInputTrigger::Pressed)]] ──
namespace fjell {
enum class EInputTrigger { Triggered, Pressed, Released, Completed };

// Mirrors InputDevice in core/input.hpp. Keep the order in step: the value
// crosses the SDK boundary as an int.
enum class EInputDevice { KeyboardMouse, Gamepad };
}

#endif

#endif // FJELL_SCRIPTING_API_H
