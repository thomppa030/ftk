#include "scripting/script_manager.hpp"
#include "scene/component_registry.hpp"
#include "scripting/script_globals.hpp"
#include "core/asset_type_registry.hpp"
#include "ui/asset_ref_widget.hpp"
#include "scripting/fjell/component.hpp"
#include "fjell/api.h"
#include "animation/anim_notify_registry.hpp"
#include "scripting/fjell/ui_component.hpp"
#include "scripting/fjell/ui_registry.hpp"
#include "scripting/fjell/weak_ref.hpp"
#include "scripting/fjell/ref.hpp"
#include "scripting/gc_visitor.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/string_utils.hpp"
#include "core/input_action_map.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "physics/physics_world.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/scene.hpp"

extern "C" void fjell_input_clear_connections();

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>

// ── Type registry (engine-side, filled by game .so) ─────────────────────

struct FjellTypeRegistry {
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

static OwnedFieldDesc copy_field(const FjellFieldDesc& fd) {
    OwnedFieldDesc ofd;
    ofd.name = fd.name;
    ofd.type = fd.type;
    ofd.offset = fd.offset;
    ofd.size = fd.size;
    ofd.min = fd.min;
    ofd.max = fd.max;
    ofd.speed = fd.speed;
    ofd.readonly = fd.readonly != 0;
    ofd.replicated = fd.replicated != 0;
    if (fd.ref_type_name) {
        ofd.ref_type_name = fd.ref_type_name;
    }
    if (fd.tooltip) {
        ofd.tooltip = fd.tooltip;
    }
    if (fd.category) {
        ofd.category = fd.category;
    }
    if (fd.type == FJELL_FIELD_ENUM && fd.enum_labels && fd.size > 0) {
        for (uint32_t e = 0; e < fd.size; ++e) {
            if (fd.enum_labels[e]) {
                ofd.enum_labels.emplace_back(fd.enum_labels[e]);
            }
        }
    }
    return ofd;
}

/// Copy a generated field table into `out`, stopping at the first null name.
///
/// A null name means `field_count` overruns the descriptor array, which happens
/// when register_all.cpp's hardcoded count is older than the regenerated table.
/// Walking off the end segfaults somewhere unrelated, so stop at the boundary
/// and name the type whose codegen is stale.
static void copy_fields(const FjellFieldDesc* fields, uint32_t count,
                        const char* type_name,
                        std::vector<OwnedFieldDesc>& out) {
    for (uint32_t i = 0; i < count; ++i) {
        if (fields == nullptr || fields[i].name == nullptr) {
            FJELL_CORE_ERROR(
                "Script '{}' declares {} reflected fields but its table ends at "
                "{} — generated code is stale. Rebuild scripts from the editor.",
                type_name ? type_name : "<unnamed>", count, i);
            return;
        }
        out.push_back(copy_field(fields[i]));
    }
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

extern "C" FJELL_SDK_EXPORT void fjell_reg_component(FjellTypeRegistry* reg, const FjellComponentTypeOOP* desc) {
    if (!reg || !desc) return;
    fjell::OwnedScriptType owned;
    owned.name = desc->name;
    owned.type_id = fjell::ScriptManager::hash_type_name(owned.name);
    owned.create = desc->create;
    owned.trace = desc->trace;
    owned.loaded = true;
    owned.is_functional_test = desc->is_functional_test;
    fjell::copy_fields(desc->fields, desc->field_count, desc->name, owned.fields);
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
    fjell::copy_fields(desc->fields, desc->field_count, desc->name, info.fields);
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
    fjell::copy_fields(desc->fields, desc->field_count, desc->name, owned.fields);
    for (uint32_t i = 0; i < desc->action_count; ++i) {
        fjell::OwnedUiActionDesc a;
        a.name = desc->actions[i].name;
        a.param_type = static_cast<uint8_t>(desc->actions[i].param_type);
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

    int total = static_cast<int>(script_type_list_.size());
    FJELL_CORE_INFO("Scripts loaded: {} type(s)", total);
    return {};
}

void ScriptManager::unload(Scene& scene) {
    if (!library_.is_loaded()) return;

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
    unload(scene);

    auto result = load(so_path);
    if (!result) {
        return make_error("Reload failed — scripts unloaded: " + result.error());
    }

    deserialize_all(scene);

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

    FjellTypeRegistry registry;
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
    entry.instance.reset(type->create());
    entry.instance->node_ = &obj;
    entry.instance->world_ = &world_;
    entry.instance->transform_ = &obj.transform;
    entry.instance->id_ = obj.id;
    entry.instance->name_ = obj.name.c_str();

    obj.scripts.push_back(std::move(entry));
    return {};
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
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (comp.instance) {
                comp.instance->world_ = &world_;
                try {
                    comp.instance->tick(dt);
                } catch (const std::exception& e) {
                    FJELL_CORE_ERROR("Exception in {}.tick on '{}': {}",
                                     comp.type_name, obj->name, e.what());
                } catch (...) {
                    FJELL_CORE_ERROR("Unknown exception in {}.tick on '{}'",
                                     comp.type_name, obj->name);
                }
            }
        }
    }

    // Tick timers only when unpaused (timers freeze during pause)
    if (dt > 0.0f) {
        timers_.tick(dt);
    }
}

// ── Lifecycle ───────────────────────────────────────────────────────────

void ScriptManager::call_begin_play(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("script_begin_play");
    // begin_play() may spawn nodes (e.g. a server spawning a player on connect);
    // defer those adds so they don't invalidate this loop's iterator, then flush
    // them once the loop completes. Same contract as update().
    {
        Scene::TickLock tick_lock(scene);
        for (auto& obj : scene.objects()) {
            for (auto& comp : obj->scripts) {
                if (comp.instance) {
                    comp.instance->world_ = &world_;
                    try {
                        comp.instance->begin_play();
                    } catch (const std::exception& e) {
                        FJELL_CORE_ERROR("Exception in {}.begin_play on '{}': {}",
                                         comp.type_name, obj->name, e.what());
                    } catch (...) {
                        FJELL_CORE_ERROR("Unknown exception in {}.begin_play on '{}'",
                                         comp.type_name, obj->name);
                    }
                }
            }
        }
    }
    // Nodes spawned during begin_play need their own script lifecycle.
    initialize_spawned(scene, scene.flush_spawned());
}

void ScriptManager::initialize_spawned(Scene& scene, const std::vector<uint32_t>& ids) {
    if (ids.empty() || !is_loaded()) return;

    // Spawning can cascade (begin_play spawns more), so defer adds and re-flush.
    std::vector<uint32_t> to_init = ids;
    while (!to_init.empty()) {
        // Instantiate scripts for this batch of new nodes (mirrors deserialize_all).
        for (uint32_t node_id : to_init) {
            Node* obj = scene.find(node_id);
            auto* spatial = obj ? obj->as_spatial() : nullptr;
            if (!spatial) continue;
            for (auto& comp : obj->scripts) {
                if (comp.instance) continue;  // already live
                const auto* ctype = find_script_type(comp.type_name);
                if (!ctype || !ctype->loaded || !ctype->create) continue;

                comp.instance.reset(ctype->create());
                comp.instance->node_ = spatial;
                comp.instance->world_ = &world_;
                comp.instance->transform_ = &spatial->transform;
                comp.instance->id_ = obj->id;
                comp.instance->name_ = obj->name.c_str();
                if (!comp.serialized.empty()) {
                    auto_deserialize_fields(ctype->fields, comp.instance.get(), comp.serialized);
                }
            }
        }

        // begin_play this batch; their spawns defer behind the lock.
        std::vector<uint32_t> next;
        {
            Scene::TickLock tick_lock(scene);
            for (uint32_t node_id : to_init) {
                Node* obj = scene.find(node_id);
                if (!obj) continue;
                for (auto& comp : obj->scripts) {
                    if (!comp.instance) continue;
                    comp.instance->world_ = &world_;
                    try {
                        comp.instance->begin_play();
                    } catch (const std::exception& e) {
                        FJELL_CORE_ERROR("Exception in {}.begin_play on '{}': {}",
                                         comp.type_name, obj->name, e.what());
                    } catch (...) {
                        FJELL_CORE_ERROR("Unknown exception in {}.begin_play on '{}'",
                                         comp.type_name, obj->name);
                    }
                }
            }
        }
        next = scene.flush_spawned();
        to_init = std::move(next);
    }
}

void ScriptManager::call_end_play(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("script_end_play");
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (comp.instance) {
                try {
                    comp.instance->end_play();
                } catch (const std::exception& e) {
                    FJELL_CORE_ERROR("Exception in {}.end_play on '{}': {}",
                                     comp.type_name, obj->name, e.what());
                } catch (...) {
                    FJELL_CORE_ERROR("Unknown exception in {}.end_play on '{}'",
                                     comp.type_name, obj->name);
                }
            }
        }
    }
    timers_.clear();
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
    // Collision enter
    for (const auto& [node_id, event] : physics.collision_enter_events()) {
        auto* node = scene.find(node_id);
        if (!node) continue;
        for (auto& comp : node->scripts) {
            if (comp.instance) {
                comp.instance->on_collision_enter(event.other_id, event.contact_point, event.contact_normal);
            }
        }
    }

    // Collision exit
    for (const auto& [node_id, event] : physics.collision_exit_events()) {
        auto* node = scene.find(node_id);
        if (!node) continue;
        for (auto& comp : node->scripts) {
            if (comp.instance) {
                comp.instance->on_collision_exit(event.other_id);
            }
        }
    }

    // Trigger enter
    for (const auto& [node_id, event] : physics.trigger_enter_events()) {
        auto* node = scene.find(node_id);
        if (!node) continue;
        for (auto& comp : node->scripts) {
            if (comp.instance) {
                comp.instance->on_trigger_enter(event.other_id);
            }
        }
    }

    // Trigger exit
    for (const auto& [node_id, event] : physics.trigger_exit_events()) {
        auto* node = scene.find(node_id);
        if (!node) continue;
        for (auto& comp : node->scripts) {
            if (comp.instance) {
                comp.instance->on_trigger_exit(event.other_id);
            }
        }
    }
}

void ScriptManager::dispatch_weather_changed(Scene& scene, uint32_t event, int phase) {
    const auto weather_phase = static_cast<WeatherPhase>(phase);
    for (const auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (comp.instance) comp.instance->on_weather_changed(event, weather_phase);
        }
    }
}

void ScriptManager::dispatch_time_phase_changed(Scene& scene, int phase) {
    const auto time_phase = static_cast<TimePhase>(phase);
    for (const auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (comp.instance) comp.instance->on_time_phase_changed(time_phase);
        }
    }
}

void ScriptManager::dispatch_player_connected(Scene& scene, uint32_t peer) {
    for (const auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (comp.instance) comp.instance->on_player_connected(peer);
        }
    }
}

void ScriptManager::dispatch_player_disconnected(Scene& scene, uint32_t peer) {
    for (const auto& obj : scene.objects()) {
        for (auto& comp : obj->scripts) {
            if (comp.instance) comp.instance->on_player_disconnected(peer);
        }
    }
}

// ── Auto-serialization from field descriptors ───────────────────────────

void ScriptManager::auto_serialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                           const void* data, nlohmann::json& out) const {
    const auto* base = static_cast<const uint8_t*>(data);
    for (const auto& f : fields) {
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
        }
    }
}

void ScriptManager::auto_deserialize_fields(const std::vector<OwnedFieldDesc>& fields,
                                              void* data, const nlohmann::json& in) const {
    auto* base = static_cast<uint8_t*>(data);
    for (const auto& f : fields) {
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
                    if (!path.empty()) {
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

            comp.instance.reset(ctype->create());
            comp.instance->node_ = spatial;
            comp.instance->world_ = &world_;
            comp.instance->transform_ = &spatial->transform;
            comp.instance->id_ = obj->id;
            comp.instance->name_ = obj->name.c_str();

            if (!comp.serialized.empty()) {
                auto_deserialize_fields(ctype->fields, comp.instance.get(), comp.serialized);
            }
        }
    }
}

// ── Inspector ───────────────────────────────────────────────────────────

static void draw_fields_inspector(const std::vector<OwnedFieldDesc>& fields, void* base_ptr) {
    auto* base = static_cast<uint8_t*>(base_ptr);
    std::string current_category;
    bool category_open = true;
    bool in_tree = false;  // true if current_category was opened via TreeNode
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];

        if (f.category != current_category) {
            if (in_tree) { ImGui::TreePop(); in_tree = false; }
            current_category = f.category;
            if (current_category.empty()) {
                category_open = true;
            } else {
                category_open = ImGui::TreeNodeEx(current_category.c_str(),
                                                  ImGuiTreeNodeFlags_DefaultOpen |
                                                  ImGuiTreeNodeFlags_SpanAvailWidth);
                in_tree = category_open;
            }
        }
        if (!category_open) continue;

        void* ptr = base + f.offset;
        float spd = f.speed > 0.0f ? f.speed : 0.1f;
        std::string label = display_name(f.name);
        const char* lbl = label.c_str();

        ImGui::PushID(static_cast<int>(i));
        if (f.readonly) ImGui::BeginDisabled();
        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                ImGui::DragFloat(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_INT:
                ImGui::DragInt(lbl, static_cast<int*>(ptr),
                               spd, static_cast<int>(f.min), static_cast<int>(f.max));
                break;
            case FJELL_FIELD_BOOL:
                ImGui::Checkbox(lbl, static_cast<bool*>(ptr));
                break;
            case FJELL_FIELD_VEC2:
                ImGui::DragFloat2(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_VEC3:
                ImGui::DragFloat3(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_VEC4:
                ImGui::DragFloat4(lbl, static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_COLOR3:
                ImGui::ColorEdit3(lbl, static_cast<float*>(ptr));
                break;
            case FJELL_FIELD_COLOR4:
                ImGui::ColorEdit4(lbl, static_cast<float*>(ptr));
                break;
            case FJELL_FIELD_STRING:
                ImGui::InputText(lbl, static_cast<char*>(ptr), f.size);
                break;
            case FJELL_FIELD_STD_STRING:
                ImGui::InputText(lbl, static_cast<std::string*>(ptr));
                break;
            case FJELL_FIELD_ENUM:
                if (!f.enum_labels.empty()) {
                    std::string preview = f.enum_labels[
                        std::clamp(*static_cast<int*>(ptr), 0,
                                   static_cast<int>(f.enum_labels.size()) - 1)];
                    if (ImGui::BeginCombo(lbl, preview.c_str())) {
                        for (int e = 0; e < static_cast<int>(f.enum_labels.size()); ++e) {
                            if (ImGui::Selectable(f.enum_labels[e].c_str(),
                                                  *static_cast<int*>(ptr) == e)) {
                                *static_cast<int*>(ptr) = e;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                break;
            case FJELL_FIELD_WEAK_REF: {
                auto* ref = static_cast<WeakRefData*>(ptr);
                const char* target_name = "(null)";
                if (ref->id != 0 && s_services.scene) {
                    auto* node = s_services.scene->find(ref->id);
                    if (node && s_services.scene->generation_of(ref->id) == ref->generation) {
                        target_name = node->name.c_str();
                    } else {
                        target_name = "(stale)";
                    }
                }
                ImGui::Text("%s: %s", lbl, target_name);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    *ref = {};
                }
                break;
            }
            case FJELL_FIELD_REF: {
                auto* ref = static_cast<RefData*>(ptr);
                const auto* asset_entry = AssetTypeRegistry::instance().find_by_name(f.ref_type_name);

                if (asset_entry && s_services.resources) {
                    // Reverse lookup current path
                    std::string current_path;
                    if (ref->handle_id != 0 && asset_entry->path_of) {
                        current_path = asset_entry->path_of(*s_services.resources, ref->handle_id);
                    }

                    const std::string exts[] = {asset_entry->extension};
                    InspectorContext sctx{.resource_manager = s_services.resources};
                    if (asset_ref_field({.label = lbl, .id = "##ref_field",
                                         .extensions = exts, .ctx = &sctx,
                                         .asset_type = asset_entry}, current_path)) {
                        // Path changed — load the asset
                        if (current_path.empty()) {
                            *ref = {};
                        } else {
                            auto resolved = base_path::resolve_asset(
                                current_path, s_services.project);
                            auto result = s_services.resources->load_asset_by_type(
                                f.ref_type_name, resolved);
                            if (result) {
                                ref->handle_id = *result;
                                ref->asset_type_id = asset_entry->type_id;
                            } else {
                                FJELL_CORE_ERROR("Asset ref: {}", result.error());
                            }
                        }
                    }
                } else {
                    ImGui::Text("%s: handle %u", lbl, ref->handle_id);
                }
                break;
            }
            case FJELL_FIELD_WIDGET_PTR: {
                auto* widget = *static_cast<UiComponent**>(ptr);
                ImGui::Text("%s: %s", lbl,
                            widget ? f.ref_type_name.c_str() : "(unbound)");
                break;
            }
        }
        if (f.readonly) ImGui::EndDisabled();
        if (!f.tooltip.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", f.tooltip.c_str());
        }
        ImGui::PopID();
    }
    if (in_tree) ImGui::TreePop();
}


void ScriptManager::draw_inspector(Node3D& obj) {
    std::string remove_name;
    for (auto& comp : obj.scripts) {
        const auto* ctype = find_script_type(comp.type_name);
        const char* label = ctype ? ctype->name.c_str() : comp.type_name.c_str();

        ImGui::PushID(comp.type_name.c_str());
        bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove")) {
                remove_name = comp.type_name;
            }
            ImGui::EndPopup();
        }

        if (open && comp.instance && ctype && ctype->loaded) {
            draw_fields_inspector(ctype->fields, comp.instance.get());
        } else if (open && (!ctype || !ctype->loaded)) {
            ImGui::TextDisabled("Script not loaded");
        }
        ImGui::PopID();
    }

    if (!remove_name.empty()) {
        remove_script(obj, remove_name);
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
