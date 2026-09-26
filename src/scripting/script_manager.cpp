#include "scripting/script_manager.hpp"
#include "scene/component_registry.hpp"
#include "scripting/script_globals.hpp"
#include "core/asset_type_registry.hpp"
#include "scripting/fjell/component.hpp"
#include "fjell/api.h"
#include "animation/anim_notify_registry.hpp"
#include "scripting/fjell/ui_component.hpp"
#include "scripting/fjell/ui_registry.hpp"
#include "scripting/fjell/weak_ref.hpp"
#include "scripting/fjell/ref.hpp"
#include "scripting/gc_visitor.hpp"
#include "scripting/input_bindings.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "physics/physics_world.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/scene.hpp"

extern "C" void fjell_input_clear_connections();

#include <string_view>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <ctime>
#include <filesystem>

// ── Type registry (engine-side, filled by game .so) ─────────────────────

struct FjellTypeRegistry {
    fjell::ReflectedTypes* types{nullptr}; // the ScriptManager's: owns the module's structs
    std::vector<fjell::OwnedScriptType> scripts;
    std::vector<fjell::OwnedUiComponentType> ui_components;
    std::vector<fjell::NotifyTypeInfo> notifies;
};

// ── Convenience aliases for shared script services ─────────────────────

static auto& s_services = fjell::script_services();

extern "C" FJELL_SDK_EXPORT void* fjell_resolve_weak_ref(uint32_t id, uint32_t generation) {
    if (!s_services.scene || id == 0) return nullptr;
    if (s_services.scene->generation_of(id) != generation) return nullptr;
    return s_services.scene->find_spatial(id);
}

// ── Ref<T> resolution (called from game .so) ───────────────────────────

extern "C" FJELL_SDK_EXPORT void* fjell_ref_resolve(uint32_t handle_id, uint8_t asset_type_id) {
    if (handle_id == 0 || asset_type_id == 0) return nullptr;
    auto* rm = s_services.resources;
    if (!rm) return nullptr;
    auto* entry = fjell::AssetTypeRegistry::instance().find_by_id(asset_type_id);
    if (!entry || !entry->resolve) return nullptr;
    return entry->resolve(*rm, handle_id);
}

// ── Legacy stubs (kept so old .so files don't crash on dlopen) ──────────

extern "C" FJELL_SDK_EXPORT void fjell_shared_ref_acquire(uint32_t) {}
extern "C" FJELL_SDK_EXPORT void fjell_shared_ref_release(uint32_t) {}
extern "C" FJELL_SDK_EXPORT void* fjell_shared_ref_resolve(uint32_t) { return nullptr; }

// ── GC trace bridge functions (called from generated code in game .so) ──

static fjell::GcVisitor* s_active_visitor = nullptr;

extern "C" FJELL_SDK_EXPORT void fjell_gc_trace_weak(void* /*visitor*/, void* weak_ref_data) {
    if (s_active_visitor && weak_ref_data) {
        s_active_visitor->trace_weak(*static_cast<fjell::WeakRefData*>(weak_ref_data));
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_gc_trace_ref(void*, void*) {}
extern "C" FJELL_SDK_EXPORT void fjell_gc_trace_shared(void*, void*) {}

namespace fjell {

// ── Running script hooks ────────────────────────────────────────────────

/// Run a hook on one of the node's scripts, reporting an exception rather
/// than letting it out: one script's mistake must not stop the others. The
/// entry is found again by index for the report, since a hook may add a
/// script to its own node and move the list.
template <typename Hook>
static void run_hook_on(Node& node, std::size_t index, const char* hook_name, Hook&& hook) {
    ScriptComponent* instance = node.scripts[index].instance.get();
    if (instance == nullptr) return;
    try {
        hook(*instance);
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("Exception in {}.{} on '{}': {}", node.scripts[index].type_name,
                         hook_name, node.name, e.what());
    } catch (...) {
        FJELL_CORE_ERROR("Unknown exception in {}.{} on '{}'", node.scripts[index].type_name,
                         hook_name, node.name);
    }
}

/// Run a hook on each script the node has when the pass reaches it, by
/// index: a hook may add a script to its own node, which a range-for over
/// the list would not survive. A script added that way was started when it
/// was added and joins the next pass, so it never runs a hook twice.
template <typename Hook>
static void run_hook(Node& node, const char* hook_name, Hook&& hook) {
    const std::size_t count = node.scripts.size();
    for (std::size_t i = 0; i < count; ++i) run_hook_on(node, i, hook_name, hook);
}

} // namespace fjell

// ── C exports called by game .so during registration ────────────────────

// ── Generated reflection table (set by register_all.cpp before registration) ──

static const FjellGeneratedInfo* s_generated_table = nullptr;
static uint32_t s_generated_count = 0;

extern "C" FJELL_SDK_EXPORT void fjell_set_generated_table(const FjellGeneratedInfo* table, uint32_t count) {
    s_generated_table = table;
    s_generated_count = count;
}

extern "C" FJELL_SDK_EXPORT const FjellGeneratedInfo* fjell_find_generated(const char* name) {
    if (!s_generated_table || !name) return nullptr;
    for (uint32_t i = 0; i < s_generated_count; ++i) {
        if (std::strcmp(s_generated_table[i].name, name) == 0) {
            return &s_generated_table[i];
        }
    }
    return nullptr;
}

extern "C" FJELL_SDK_EXPORT void fjell_reg_types(FjellTypeRegistry* reg, const FjellTypeDesc* const* types,
                                                 uint32_t count) {
    if (!reg || !reg->types) return;
    reg->types->add_types(types, count);
}

extern "C" FJELL_SDK_EXPORT void fjell_reg_pipes(FjellTypeRegistry* reg, const FjellPipeDesc* pipes, uint32_t count) {
    if (!reg || !reg->types) return;
    reg->types->add_pipes(pipes, count);
}

extern "C" FJELL_SDK_EXPORT void fjell_reg_component(FjellTypeRegistry* reg, const FjellComponentTypeOOP* desc) {
    if (!reg || !desc) return;
    fjell::OwnedScriptType owned;
    owned.name = desc->name;
    owned.type_id = fjell::ScriptManager::hash_type_name(owned.name);
    owned.create = desc->create;
    owned.trace = desc->trace;
    owned.loaded = true;
    owned.is_functional_test = desc->is_functional_test;
    reg->types->copy_fields(desc->fields, desc->field_count, desc->name, owned.fields);
    // Attach the generated RPC table (same .so), if this type declares any.
    if (const auto* gen = fjell_find_generated(desc->name)) {
        owned.rpcs = gen->rpcs;
        owned.rpc_count = gen->rpc_count;
    }
    reg->scripts.push_back(std::move(owned));
}

extern "C" FJELL_SDK_EXPORT void fjell_reg_notify(FjellTypeRegistry* reg, const FjellNotifyTypeOOP* desc) {
    if (!reg || !desc) return;
    fjell::NotifyTypeInfo info;
    info.name = desc->name;
    info.create = [create = desc->create]() -> std::unique_ptr<fjell::AnimNotify> {
        return std::unique_ptr<fjell::AnimNotify>(create());
    };
    info.trace = desc->trace;
    info.is_state = desc->is_state;
    reg->types->copy_fields(desc->fields, desc->field_count, desc->name, info.fields);
    reg->notifies.push_back(std::move(info));
}

// UI generated reflection table (parallel to script table)
static const FjellUiGeneratedInfo* s_ui_generated_table = nullptr;
static uint32_t s_ui_generated_count = 0;

extern "C" FJELL_SDK_EXPORT void fjell_set_ui_generated_table(const FjellUiGeneratedInfo* table, uint32_t count) {
    s_ui_generated_table = table;
    s_ui_generated_count = count;
}

extern "C" FJELL_SDK_EXPORT const FjellUiGeneratedInfo* fjell_find_ui_generated(const char* name) {
    if (!s_ui_generated_table || !name) return nullptr;
    for (uint32_t i = 0; i < s_ui_generated_count; ++i) {
        if (std::strcmp(s_ui_generated_table[i].name, name) == 0) {
            return &s_ui_generated_table[i];
        }
    }
    return nullptr;
}

extern "C" FJELL_SDK_EXPORT void fjell_reg_ui_component(FjellTypeRegistry* reg, const FjellUiWidgetTypeOOP* desc) {
    if (!reg || !desc) return;
    fjell::OwnedUiWidgetType owned;
    owned.name = desc->name;
    owned.create = desc->create;
    owned.trace = desc->trace;
    owned.loaded = true;
    reg->types->copy_fields(desc->fields, desc->field_count, desc->name, owned.fields);
    for (uint32_t i = 0; i < desc->action_count; ++i) {
        fjell::OwnedUiActionDesc a;
        a.name = desc->actions[i].name;
        a.param = desc->actions[i].param_type;
        a.invoke = desc->actions[i].invoke;
        owned.actions.push_back(std::move(a));
    }
    reg->ui_components.push_back(std::move(owned));
}

namespace fjell {

// ── FNV-1a hash ─────────────────────────────────────────────────────────

uint32_t ScriptManager::hash_type_name(const std::string& name) {
    uint32_t hash = 2166136261u;
    for (char c : name) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

// ── Constructor / Destructor ────────────────────────────────────────────

ScriptManager::ScriptManager() = default;

ScriptManager::~ScriptManager() {
    if (library_.is_loaded()) on_unloading.broadcast();
    library_.unload();
    if (!loaded_temp_path_.empty()) {
        std::filesystem::remove(loaded_temp_path_);
    }
}

// ── Load / Unload / Reload ──────────────────────────────────────────────

Result<> ScriptManager::load(const std::filesystem::path& so_path) {
    if (!std::filesystem::exists(so_path)) {
        return make_error("Script library not found: " + so_path.string());
    }

    // Clean up stale temp .so files from previous loads or crashes
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(so_path.parent_path())) {
        if (entry.is_regular_file()) {
            auto name = entry.path().filename().string();
            if (name.starts_with("game_") && name.ends_with(platform::SHARED_LIB_EXT) &&
                entry.path() != loaded_temp_path_) {
                fs::remove(entry.path());
            }
        }
    }

    static uint32_t s_reload_counter = 0;
    auto temp = so_path.parent_path() /
        ("game_" + std::to_string(++s_reload_counter) + platform::SHARED_LIB_EXT);
    std::filesystem::copy_file(so_path, temp,
                               std::filesystem::copy_options::overwrite_existing);

    if (!library_.load(temp)) {
        std::filesystem::remove(temp);
        return make_error("Failed to load script library: " + library_.error());
    }

    so_path_ = so_path;

    // Delete old temp after new one is loaded (old dlopen handle is already closed by unload())
    if (!loaded_temp_path_.empty() && loaded_temp_path_ != temp) {
        std::filesystem::remove(loaded_temp_path_);
    }
    loaded_temp_path_ = temp;

    build_registry();
    ++library_revision_;

    int total = static_cast<int>(script_type_list_.size());
    FJELL_CORE_INFO("Scripts loaded: {} type(s)", total);
    return {};
}

void ScriptManager::unload(Scene& scene) {
    if (!library_.is_loaded()) return;
    on_unloading.broadcast();

    serialize_all(scene);

    // Clear input bindings and timers before destroying instances — the lambdas
    // capture function pointers into the .so and `this` pointers to script
    // instances, both of which become dangling after unload.
    fjell_input_clear_connections();
    timers_.clear();

    for (auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            comp.instance.reset();
        }
    }

    library_.unload();

    for (auto& t : script_type_list_) t.loaded = false;
}

Result<> ScriptManager::reload(Scene& scene, const std::filesystem::path& so_path) {
    FJELL_CORE_INFO("Hot-reloading scripts...");
    // In a running game a reload is the scripts ending and new ones starting:
    // the ones going away get their end_play, and the new ones their
    // begin_play, which is where they bind input again.
    if (playing_) call_end_play(scene);
    unload(scene);

    auto result = load(so_path);
    if (!result) {
        return make_error("Reload failed — scripts unloaded: " + result.error());
    }

    deserialize_all(scene);
    if (playing_) call_begin_play(scene);

#ifndef NDEBUG
    last_reload_time_ = std::chrono::steady_clock::now();
#endif

    FJELL_CORE_INFO("Scripts reloaded");
    return {};
}

// ── Registry building ───────────────────────────────────────────────────

void ScriptManager::build_registry() {
    script_type_list_.clear();
    script_type_map_.clear();

    // Check API version compatibility
    using VersionFn = uint32_t (*)();
    auto* version_fn = library_.symbol_as<VersionFn>("fjell_api_version");
    if (version_fn) {
        uint32_t so_version = version_fn();
        if (so_version != FJELL_API_VERSION) {
            FJELL_CORE_ERROR("Script .so API version mismatch: engine={}, .so={} — recompile scripts",
                             FJELL_API_VERSION, so_version);
            return;
        }
    } else {
        FJELL_CORE_WARN("No fjell_api_version symbol — .so may be outdated");
    }

    // The version catches deliberate ABI changes; this catches the mirrored
    // component structs drifting from the engine's, which no one bumps for.
    using LayoutFn = uint32_t (*)(const FjellComponentLayout**);
    if (auto* layout_fn = library_.symbol_as<LayoutFn>("fjell_engine_component_layouts")) {
        const FjellComponentLayout* layouts = nullptr;
        uint32_t count = layout_fn(&layouts);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* meta = ComponentRegistry::instance().find_by_script_name(layouts[i].type_name);
            if (meta == nullptr) continue;
            if (meta->type_size != layouts[i].size) {
                FJELL_CORE_ERROR("Script .so was built against a different {} layout "
                                 "({} bytes, engine has {}) — recompile scripts",
                                 layouts[i].type_name, layouts[i].size, meta->type_size);
                return;
            }
        }
    } else {
        FJELL_CORE_WARN("No fjell_engine_component_layouts symbol — .so may be outdated");
    }

    using RegFn = void (*)(FjellTypeRegistry*);
    auto* reg_fn = library_.symbol_as<RegFn>("fjell_register");
    if (!reg_fn) {
        FJELL_CORE_WARN("No fjell_register symbol found in .so");
        return;
    }

    reflected_types_.clear();
    FjellTypeRegistry registry;
    registry.types = &reflected_types_;
    reg_fn(&registry);

    for (auto& ct : registry.scripts) {
        script_type_map_[ct.type_id] = script_type_list_.size();
        script_type_list_.push_back(std::move(ct));
    }

    ui_type_list_.clear();
    for (auto& ut : registry.ui_components) {
        ui_type_list_.push_back(std::move(ut));
    }

    for (auto& nt : registry.notifies) {
        AnimNotifyRegistry::instance().register_notify(std::move(nt));
    }
}

// ── Type lookups ────────────────────────────────────────────────────────

const OwnedScriptType* ScriptManager::find_script_type(const std::string& name) const {
    auto id = hash_type_name(name);
    auto it = script_type_map_.find(id);
    if (it == script_type_map_.end()) return nullptr;
    return &script_type_list_[it->second];
}

const OwnedUiWidgetType* ScriptManager::find_ui_widget_type(const std::string& name) const {
    for (const auto& ut : ui_type_list_) {
        if (ut.name == name) return &ut;
    }
    return nullptr;
}

const OwnedUiWidgetType* ScriptManager::find_ui_type_by_tag(const std::string& tag) const {
    return find_ui_widget_type(tag); // backward compat — just match by name
}

// ── Component management ────────────────────────────────────────────────

Result<> ScriptManager::add_script(Node3D& obj, const std::string& type_name) {
    const auto* type = find_script_type(type_name);
    if (!type || !type->loaded || !type->create) {
        return make_error("Unknown or unloaded script type: " + type_name);
    }

    ScriptEntry entry;
    entry.type_name = type->name;
    instantiate(obj, entry);
    obj.scripts.push_back(std::move(entry));

    // Added to a running game, it starts now, as it would have with the rest
    // at play start.
    if (playing_) start_script(obj, obj.scripts.size() - 1);
    return {};
}

void ScriptManager::instantiate(Node3D& obj, ScriptEntry& entry) {
    const auto* type = find_script_type(entry.type_name);
    if (!type || !type->loaded || !type->create) {
        entry.instance.reset();
        return;
    }
    entry.instance.reset(type->create());
    entry.started = false;
    entry.instance->node_ = &obj;
    entry.instance->world_ = &world_;
    entry.instance->transform_ = &obj.transform;
    entry.instance->id_ = obj.id;
    entry.instance->name_ = obj.name.c_str();
    if (!entry.serialized.empty()) {
        auto_deserialize_fields(type->fields, entry.instance.get(), entry.serialized);
    }
}

nlohmann::json ScriptManager::fields_of(const ScriptEntry& entry) const {
    const auto* type = find_script_type(entry.type_name);
    if (!entry.instance || !type) return entry.serialized;
    nlohmann::json fields = nlohmann::json::object();
    auto_serialize_fields(type->fields, entry.instance.get(), fields);
    return fields;
}

void ScriptManager::set_fields(ScriptEntry& entry, const nlohmann::json& fields) {
    entry.serialized = fields;
    const auto* type = find_script_type(entry.type_name);
    if (entry.instance && type) {
        auto_deserialize_fields(type->fields, entry.instance.get(), fields);
    }
}

void ScriptManager::remove_script(Node3D& obj, const std::string& type_name) {
    auto it = std::find_if(obj.scripts.begin(), obj.scripts.end(),
                           [&type_name](const ScriptEntry& c) { return c.type_name == type_name; });
    if (it == obj.scripts.end()) return;
    obj.scripts.erase(it);
}

// ── World population ────────────────────────────────────────────────────

void ScriptManager::populate_world(const Input& input, const Clock& clock,
                                   PhysicsWorld* physics) {
    world_.input_ = &input;
    s_services.physics = physics;
    world_.delta_time = clock.game_delta_time();
    world_.elapsed_time = clock.game_elapsed();
}

// ── Writes the simulation owns ──────────────────────────────────────────

bool ScriptManager::simulation_owns(const Node& node) {
    if (const auto* rb = node.components.get<RigidbodyComponent>();
        rb != nullptr && rb->body_type == BodyType::Dynamic) {
        return true;
    }
    const auto* cc = node.components.get<CharacterComponent>();
    return cc != nullptr && cc->simulate;
}

ScriptManager::SimulatedState ScriptManager::simulated_state(const Node& node) {
    SimulatedState state;
    if (const auto* spatial = node.as_spatial()) {
        state.position = spatial->transform.position;
        state.rotation = spatial->transform.rotation;
    }
    if (const auto* cc = node.components.get<CharacterComponent>()) state.velocity = cc->velocity;
    return state;
}

namespace {

/// What of the state the simulation owns a frame hook changed, by the
/// simulation's rules: a dynamic body's placement, an Intent character's
/// horizontal velocity, a Velocity character's velocity. A character's own
/// placement is not among them: moved from outside, it is taken as a teleport.
enum class OutsideWrite : uint8_t { none, body_placement, intent_velocity, character_velocity };

template <typename State>
[[nodiscard]] OutsideWrite outside_write(const Node& node, const State& before, const State& now) {
    if (const auto* rb = node.components.get<RigidbodyComponent>();
        rb != nullptr && rb->body_type == BodyType::Dynamic &&
        (now.position != before.position || now.rotation != before.rotation)) {
        return OutsideWrite::body_placement;
    }
    const auto* cc = node.components.get<CharacterComponent>();
    if (cc == nullptr || !cc->simulate) return OutsideWrite::none;
    if (cc->movement == CharacterMovement::Intent) {
        return now.velocity.x != before.velocity.x || now.velocity.z != before.velocity.z
                   ? OutsideWrite::intent_velocity
                   : OutsideWrite::none;
    }
    return now.velocity != before.velocity ? OutsideWrite::character_velocity : OutsideWrite::none;
}

/// What to do instead, for the report.
[[nodiscard]] const char* outside_write_advice(OutsideWrite write) {
    switch (write) {
        case OutsideWrite::body_placement:
            return "moved a dynamic body, which physics puts back at its next step: move it "
                   "with teleport(), or push it with forces or its velocity";
        case OutsideWrite::intent_velocity:
            return "wrote the horizontal velocity of a character moved by intent, which the "
                   "engine sets from move_intent every step: set move_intent, or switch the "
                   "character's movement to Velocity";
        case OutsideWrite::character_velocity:
            return "wrote a character's velocity once a frame, which ties its movement to the "
                   "frame rate: write it in fixed_tick(), or move the character by intent";
        case OutsideWrite::none:
            break;
    }
    return "";
}

} // namespace

template <typename Hook>
void ScriptManager::run_frame_hook(Node& node, const char* hook_name, Hook&& hook) {
    if (!write_reports_on_ || !simulation_owns(node)) {
        run_hook(node, hook_name, hook);
        return;
    }
    // Checked after each script, so the report names the one that wrote.
    const std::size_t count = node.scripts.size();
    for (std::size_t i = 0; i < count; ++i) {
        const SimulatedState before = simulated_state(node);
        run_hook_on(node, i, hook_name, hook);
        report_outside_writes(node, i, hook_name, before);
    }
}

void ScriptManager::report_outside_writes(const Node& node, std::size_t script_index,
                                          const char* hook_name, const SimulatedState& before) {
    const OutsideWrite write = outside_write(node, before, simulated_state(node));
    if (write == OutsideWrite::none) return;
    const std::string& type = node.scripts[script_index].type_name;
    const std::string what = node.name + '|' + std::to_string(static_cast<int>(write));
    blamed_writes_.insert(what);
    report_write(type + '|' + what,
                 std::format("{} on '{}' {} in {}()", type, node.name,
                             outside_write_advice(write), hook_name));
}

void ScriptManager::report_write(const std::string& key, std::string message) {
    if (!reported_writes_.insert(key).second) return;
    FJELL_CORE_WARN("{}", message);
    write_reports_.push_back(std::move(message));
}

std::vector<std::string> ScriptManager::take_write_reports() {
    return std::exchange(write_reports_, {});
}

std::vector<std::pair<uint32_t, ScriptManager::SimulatedState>>
ScriptManager::owned_states(Scene& scene) const {
    std::vector<std::pair<uint32_t, SimulatedState>> states;
    if (!write_reports_on_) return states;
    for (const auto& obj : scene.objects()) {
        if (simulation_owns(*obj)) states.emplace_back(obj->id, simulated_state(*obj));
    }
    return states;
}

void ScriptManager::report_foreign_writes(Scene& scene,
                                          std::span<const std::pair<uint32_t, SimulatedState>> before,
                                          const char* hook_name) {
    for (const auto& [id, state] : before) {
        const Node* node = scene.find(id);
        if (node == nullptr) continue;
        const OutsideWrite write = outside_write(*node, state, simulated_state(*node));
        if (write == OutsideWrite::none) continue;
        // One of the node's own scripts was already named for it.
        const std::string blamed = node->name + '|' + std::to_string(static_cast<int>(write));
        if (blamed_writes_.contains(blamed)) continue;
        report_write("|" + blamed,
                     std::format("A script on another node {} ('{}') in {}()",
                                 outside_write_advice(write), node->name, hook_name));
    }
}

// ── Prepare / Update ────────────────────────────────────────────────────

void ScriptManager::prepare(Scene& scene, const Input& input, const Clock& clock,
                            PhysicsWorld* physics) {
    s_services.scene = &scene;
    s_services.manager = this;
    populate_world(input, clock, physics);
}

void ScriptManager::update(Scene& scene, const Input& input, const Clock& clock,
                           PhysicsWorld* physics) {
    FJELL_PROFILE_SCOPE_N("script_update");
    if (!is_loaded()) return;

    s_services.scene = &scene;
    s_services.manager = this;
    populate_world(input, clock, physics);

    float dt = clock.game_delta_time();

    // Scripts may spawn nodes during tick(); defer those adds so they don't
    // invalidate this loop's iterator over objects_.
    Scene::TickLock tick_lock(scene);
    const auto owned_before = owned_states(scene);
    for (auto& obj : scene.objects()) {
        run_frame_hook(*obj, "tick", [&](ScriptComponent& script) {
            script.world_ = &world_;
            script.tick(dt);
        });
    }
    report_foreign_writes(scene, owned_before, "tick");

}

void ScriptManager::fixed_update(Scene& scene, const FixedStep& step) {
    FJELL_PROFILE_SCOPE_N("script_fixed_update");
    if (!is_loaded()) return;

    // Inside the step, the time a script reads is the step's: code that asks
    // for delta_time() in fixed_tick() gets the step length, not the frame's.
    const float frame_delta = world_.delta_time;
    const float frame_elapsed = world_.elapsed_time;
    world_.delta_time = step.dt;
    world_.elapsed_time = static_cast<float>(step.time);
    world_.fixed_delta_time = step.dt;
    world_.fixed_time = step.time;
    world_.fixed_step_index = step.index;
    s_services.in_fixed_step = true;
    {
        // Spawns from fixed_tick and from timers wait for the flush that
        // follows the step.
        Scene::TickLock tick_lock(scene);
        for (auto& obj : scene.objects()) {
            run_hook(*obj, "fixed_tick", [&](ScriptComponent& script) {
                script.world_ = &world_;
                script.fixed_tick(step);
            });
        }
        timers_.tick(step.dt);
    }
    s_services.in_fixed_step = false;
    world_.delta_time = frame_delta;
    world_.elapsed_time = frame_elapsed;
}

void ScriptManager::late_update(Scene& scene, float dt) {
    FJELL_PROFILE_SCOPE_N("script_late_update");
    if (!is_loaded()) return;

    Scene::TickLock tick_lock(scene);
    const auto owned_before = owned_states(scene);
    for (auto& obj : scene.objects()) {
        run_frame_hook(*obj, "late_tick", [&](ScriptComponent& script) {
            script.world_ = &world_;
            script.late_tick(dt);
        });
    }
    report_foreign_writes(scene, owned_before, "late_tick");
}

// ── Lifecycle ───────────────────────────────────────────────────────────

void ScriptManager::start_script(Node& node, std::size_t index) {
    ScriptEntry& entry = node.scripts[index];
    if (entry.instance == nullptr || entry.started) return;
    // Marked before it runs: a begin_play that reaches back into this node
    // must not start the script a second time.
    entry.started = true;
    run_hook_on(node, index, "begin_play", [this](ScriptComponent& script) {
        script.world_ = &world_;
        script.begin_play();
    });
}

void ScriptManager::call_begin_play(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("script_begin_play");
    // begin_play() may spawn nodes (e.g. a server spawning a player on connect);
    // defer those adds so they don't invalidate this loop's iterator. They
    // are started with everything else that joins the game, by whoever
    // flushes spawns next.
    Scene::TickLock tick_lock(scene);
    for (auto& obj : scene.objects()) {
        const std::size_t count = obj->scripts.size();
        for (std::size_t i = 0; i < count; ++i) start_script(*obj, i);
    }
}

void ScriptManager::initialize_spawned(Scene& scene, const std::vector<uint32_t>& ids) {
    if (ids.empty() || !is_loaded()) return;

    // Every script is created before any begin_play runs, so a begin_play
    // can find the scripts on the other nodes that joined with its own.
    for (uint32_t node_id : ids) {
        Node* obj = scene.find(node_id);
        auto* spatial = obj ? obj->as_spatial() : nullptr;
        if (!spatial) continue;
        for (auto& comp : obj->scripts) {
            if (comp.instance) continue;  // already live
            instantiate(*spatial, comp);
        }
    }

    // What these begin_plays spawn is deferred behind the lock and left for
    // the caller's next round.
    Scene::TickLock tick_lock(scene);
    for (uint32_t node_id : ids) {
        Node* obj = scene.find(node_id);
        if (!obj) continue;
        const std::size_t count = obj->scripts.size();
        for (std::size_t i = 0; i < count; ++i) start_script(*obj, i);
    }
}

void ScriptManager::call_end_play(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("script_end_play");
    // An end_play that spawns must not move the list under this loop.
    Scene::TickLock tick_lock(scene);
    for (auto& obj : scene.objects()) {
        run_hook(*obj, "end_play", [](ScriptComponent& script) { script.end_play(); });
    }
    timers_.clear();
}

void ScriptManager::end_play_destroyed(Scene& scene, std::span<const uint32_t> doomed_ids) {
    for (uint32_t id : doomed_ids) {
        Node* obj = scene.find(id);
        if (obj == nullptr) continue;
        // By index: an end_play may add a script to its own node, which is
        // then ended in turn.
        for (std::size_t i = 0; i < obj->scripts.size(); ++i) {
            ScriptComponent* instance = obj->scripts[i].instance.get();
            if (instance == nullptr) continue;
            run_hook_on(*obj, i, "end_play", [](ScriptComponent& script) { script.end_play(); });
            release(instance);
            obj->scripts[i].instance.reset();
        }
    }
}

void ScriptManager::release_address(const void* owner) {
    release_input_bindings(owner);
    timers_.cancel_owned(owner);
}

// ── GC trace — null WeakRefs pointing at doomed nodes ───────────────────

void ScriptManager::trace_doomed_refs(Scene& scene, std::span<const uint32_t> doomed_ids) {
    if (doomed_ids.empty()) return;

    GcVisitor visitor;
    visitor.set_doomed_ids(doomed_ids);
    s_active_visitor = &visitor;

    for (auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (!comp.instance) continue;
            const auto* ctype = find_script_type(comp.type_name);
            if (ctype && ctype->trace) {
                ctype->trace(comp.instance.get(), &visitor);
            }
        }
    }

    s_active_visitor = nullptr;
}

// ── Collision/trigger event dispatch ─────────────────────────────────────

void ScriptManager::dispatch_collision_events(Scene& scene, PhysicsWorld& physics) {
    // Each event goes to the scripts on the node it happened to. The events
    // are the physics world's own lists, which no hook can change; a node a
    // hook spawns joins the scene and is started with the rest.
    auto deliver = [&](const auto& events, const char* hook_name, auto&& hook) {
        for (const auto& [node_id, event] : events) {
            Node* node = scene.find(node_id);
            if (node == nullptr) continue;
            run_hook(*node, hook_name, [&](ScriptComponent& script) { hook(script, event); });
        }
    };
    deliver(physics.collision_enter_events(), "on_collision_enter",
            [](ScriptComponent& script, const CollisionEvent& e) {
                script.on_collision_enter(e.other_id, e.contact_point, e.contact_normal);
            });
    deliver(physics.collision_exit_events(), "on_collision_exit",
            [](ScriptComponent& script, const CollisionEvent& e) {
                script.on_collision_exit(e.other_id);
            });
    deliver(physics.trigger_enter_events(), "on_trigger_enter",
            [](ScriptComponent& script, const CollisionEvent& e) {
                script.on_trigger_enter(e.other_id);
            });
    deliver(physics.trigger_exit_events(), "on_trigger_exit",
            [](ScriptComponent& script, const CollisionEvent& e) {
                script.on_trigger_exit(e.other_id);
            });
}

void ScriptManager::dispatch_weather_changed(Scene& scene, uint32_t event, int phase) {
    const auto weather_phase = static_cast<WeatherPhase>(phase);
    Scene::TickLock tick_lock(scene);
    for (const auto& obj : scene.objects()) {
        run_hook(*obj, "on_weather_changed", [&](ScriptComponent& script) {
            script.on_weather_changed(event, weather_phase);
        });
    }
}

void ScriptManager::dispatch_time_phase_changed(Scene& scene, int phase) {
    const auto time_phase = static_cast<TimePhase>(phase);
    Scene::TickLock tick_lock(scene);
    for (const auto& obj : scene.objects()) {
        run_hook(*obj, "on_time_phase_changed", [&](ScriptComponent& script) {
            script.on_time_phase_changed(time_phase);
        });
    }
}

void ScriptManager::dispatch_player_connected(Scene& scene, uint32_t peer) {
    Scene::TickLock tick_lock(scene);
    for (const auto& obj : scene.objects()) {
        run_hook(*obj, "on_player_connected",
                 [&](ScriptComponent& script) { script.on_player_connected(peer); });
    }
}

void ScriptManager::dispatch_player_disconnected(Scene& scene, uint32_t peer) {
    Scene::TickLock tick_lock(scene);
    for (const auto& obj : scene.objects()) {
        run_hook(*obj, "on_player_disconnected",
                 [&](ScriptComponent& script) { script.on_player_disconnected(peer); });
    }
}

// ── Auto-serialization from field descriptors ───────────────────────────

void ScriptManager::auto_serialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                           const void* data, nlohmann::json& out) const {
    const auto* base = static_cast<const uint8_t*>(data);
    for (const auto& f : fields) {
        if (!f.is_property()) continue; // runtime state, getters and Models are not saved
        const void* ptr = base + f.offset;
        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                out[f.name] = *static_cast<const float*>(ptr);
                break;
            case FJELL_FIELD_INT:
            case FJELL_FIELD_ENUM:
                out[f.name] = *static_cast<const int32_t*>(ptr);
                break;
            case FJELL_FIELD_BOOL:
                out[f.name] = *static_cast<const bool*>(ptr);
                break;
            case FJELL_FIELD_VEC2: {
                const auto* v = static_cast<const float*>(ptr);
                out[f.name] = {v[0], v[1]};
                break;
            }
            case FJELL_FIELD_VEC3:
            case FJELL_FIELD_COLOR3: {
                const auto* v = static_cast<const float*>(ptr);
                out[f.name] = {v[0], v[1], v[2]};
                break;
            }
            case FJELL_FIELD_VEC4:
            case FJELL_FIELD_COLOR4: {
                const auto* v = static_cast<const float*>(ptr);
                out[f.name] = {v[0], v[1], v[2], v[3]};
                break;
            }
            case FJELL_FIELD_STRING:
                out[f.name] = static_cast<const char*>(ptr);
                break;
            case FJELL_FIELD_STD_STRING:
                out[f.name] = *static_cast<const std::string*>(ptr);
                break;
            case FJELL_FIELD_WEAK_REF: {
                const auto* ref = static_cast<const WeakRefData*>(ptr);
                out[f.name] = {{"id", ref->id}, {"generation", ref->generation}};
                break;
            }
            case FJELL_FIELD_REF: {
                const auto* ref = static_cast<const RefData*>(ptr);
                std::string path;
                if (ref->handle_id != 0 && s_services.resources) {
                    const auto* entry = AssetTypeRegistry::instance().find_by_name(f.ref_type_name);
                    if (entry && entry->path_of) {
                        path = entry->path_of(*s_services.resources, ref->handle_id);
                    }
                }
                out[f.name] = {{"asset_path", path}};
                break;
            }
            case FJELL_FIELD_WIDGET_PTR:
                // Resolved at runtime by UiSubsystem, not serialized
                break;
            case FJELL_FIELD_STRUCT:
            case FJELL_FIELD_VECTOR:
                break; // never properties, skipped above
        }
    }
}

void ScriptManager::auto_deserialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                              void* data, const nlohmann::json& in) const {
    auto* base = static_cast<uint8_t*>(data);
    for (const auto& f : fields) {
        if (!f.is_property()) continue;
        void* ptr = base + f.offset;
        if (!in.contains(f.name)) continue;

        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                *static_cast<float*>(ptr) = in[f.name].get<float>();
                break;
            case FJELL_FIELD_INT:
                *static_cast<int32_t*>(ptr) = in[f.name].get<int32_t>();
                break;
            case FJELL_FIELD_ENUM: {
                int32_t val = in[f.name].get<int32_t>();
                int32_t max_val = static_cast<int32_t>(f.size) - 1;
                if (max_val >= 0) val = std::clamp(val, int32_t{0}, max_val);
                *static_cast<int32_t*>(ptr) = val;
                break;
            }
            case FJELL_FIELD_BOOL:
                *static_cast<bool*>(ptr) = in[f.name].get<bool>();
                break;
            case FJELL_FIELD_VEC2: {
                auto* v = static_cast<float*>(ptr);
                auto& a = in[f.name];
                if (a.is_array() && a.size() >= 2) { v[0] = a[0]; v[1] = a[1]; }
                break;
            }
            case FJELL_FIELD_VEC3:
            case FJELL_FIELD_COLOR3: {
                auto* v = static_cast<float*>(ptr);
                auto& a = in[f.name];
                if (a.is_array() && a.size() >= 3) { v[0] = a[0]; v[1] = a[1]; v[2] = a[2]; }
                break;
            }
            case FJELL_FIELD_VEC4:
            case FJELL_FIELD_COLOR4: {
                auto* v = static_cast<float*>(ptr);
                auto& a = in[f.name];
                if (a.is_array() && a.size() >= 4) { v[0] = a[0]; v[1] = a[1]; v[2] = a[2]; v[3] = a[3]; }
                break;
            }
            case FJELL_FIELD_STRING: {
                auto s = in[f.name].get<std::string>();
                std::strncpy(static_cast<char*>(ptr), s.c_str(), f.size - 1);
                static_cast<char*>(ptr)[f.size - 1] = '\0';
                break;
            }
            case FJELL_FIELD_STD_STRING:
                *static_cast<std::string*>(ptr) = in[f.name].get<std::string>();
                break;
            case FJELL_FIELD_WEAK_REF: {
                auto* ref = static_cast<WeakRefData*>(ptr);
                auto& j = in[f.name];
                if (j.is_object()) {
                    ref->id = j.value("id", uint32_t{0});
                    ref->generation = j.value("generation", uint32_t{0});
                }
                break;
            }
            case FJELL_FIELD_REF: {
                auto* ref = static_cast<RefData*>(ptr);
                auto& j = in[f.name];
                if (j.is_object() && j.contains("asset_path") && s_services.resources) {
                    auto path = j["asset_path"].get<std::string>();
                    if (path.empty()) {
                        *ref = {};
                    } else {
                        auto result = s_services.resources->load_asset_by_type(
                            f.ref_type_name, path);
                        if (result) {
                            ref->handle_id = *result;
                            auto* at = AssetTypeRegistry::instance().find_by_name(f.ref_type_name);
                            ref->asset_type_id = at ? at->type_id : 0;
                        }
                    }
                }
                break;
            }
            case FJELL_FIELD_WIDGET_PTR:
                // Resolved at runtime by UiSubsystem, not serialized
                break;
            case FJELL_FIELD_STRUCT:
            case FJELL_FIELD_VECTOR:
                break; // never properties, skipped above
        }
    }
}

// ── Serialize / Deserialize ─────────────────────────────────────────────

void ScriptManager::serialize_all(Scene& scene) {
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (!comp.instance) continue;
            const auto* type = find_script_type(comp.type_name);
            if (!type) continue;

            comp.serialized = nlohmann::json::object();
            auto_serialize_fields(type->fields, comp.instance.get(), comp.serialized);
        }
    }
}

void ScriptManager::deserialize_all(Scene& scene) {
    for (auto& obj : scene.objects()) {
        auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        for (auto& comp : obj->scripts) {
            const auto* ctype = find_script_type(comp.type_name);
            if (!ctype || !ctype->loaded || !ctype->create) continue;
            instantiate(*spatial, comp);
        }
    }
}

// ── Hot-reload watcher (debug only) ─────────────────────────────────────

#ifndef NDEBUG

void ScriptManager::watch(const std::filesystem::path& so_path) {
    watcher_.clear();
    so_path_ = so_path;

    auto target_name = so_path.filename();
    watcher_.watch(so_path.parent_path(), [this, target_name](const std::filesystem::path& path) {
        if (path.filename() == target_name) {
            reload_pending_ = true;
        }
    });

    FJELL_CORE_INFO("Script hot-reload watching: {}", so_path.parent_path().string());
}

Result<> ScriptManager::poll_reload(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("script_poll_reload");
    if (so_path_.empty()) return {};

    reload_pending_ = false;
    (void)watcher_.poll();

    if (reload_pending_) {
        // Suppress watcher events shortly after an explicit reload
        // to avoid double-reloading from Build & Reload + file watcher
        auto elapsed = std::chrono::steady_clock::now() - last_reload_time_;
        if (elapsed < std::chrono::seconds(2)) {
            return {};
        }
        return reload(scene, so_path_);
    }
    return {};
}

#endif // NDEBUG

} // namespace fjell
