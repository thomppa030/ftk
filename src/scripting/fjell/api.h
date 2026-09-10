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
#define FJELL_API_VERSION 17

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
    FJELL_FIELD_STRING,
    FJELL_FIELD_ENUM,
    FJELL_FIELD_WEAK_REF,
    FJELL_FIELD_REF,
    FJELL_FIELD_WIDGET_PTR     // [[fj::bind]] WidgetType* — resolved by UiSubsystem
} FjellFieldType;

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
} FjellFieldDesc;

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
}

#endif

#endif // FJELL_SCRIPTING_API_H
