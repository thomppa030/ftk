#include "scripting/script_manager.hpp"
#include "scripting/script_globals.hpp"
#include "scripting/fjell/component.hpp"
#include "scripting/fjell/weak_ref.hpp"
#include "scripting/fjell/ref.hpp"
#include "scripting/fjell/shared_ref.hpp"
#include "scripting/gc_visitor.hpp"
#include "scripting/ref_count_table.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "physics/physics_world.hpp"
#include "core/base_path.hpp"
#include "renderer/resource_manager.hpp"
#include "scene/scene.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>

// ── Type registry (engine-side, filled by game .so) ─────────────────────

struct FjellTypeRegistry {
    std::vector<fjell::OwnedScriptType> scripts;
};

// ── Asset type registry (maps ref_type_name → file extension) ───────────

struct AssetTypeInfo {
    const char* type_name;
    const char* extension;
    const char* label;
};

static constexpr AssetTypeInfo s_asset_types[] = {
    {"VfxAsset",   ".fjvfx",   "VFX Effect"},
    {"AudioAsset", ".fjaudio", "Audio Clip"},
    {"Material",   ".fjmat",   "Material"},
};

static const AssetTypeInfo* find_asset_type(const std::string& ref_type_name) {
    for (const auto& t : s_asset_types) {
        if (ref_type_name == t.type_name) return &t;
    }
    return nullptr;
}

// ── Convenience aliases for shared script globals ────────────────────────

static auto& s_active_scene = fjell::script_active_scene();
static auto& s_active_script_mgr = fjell::script_active_manager();
static auto& s_resource_manager = fjell::script_resource_manager();
static auto& s_active_project = fjell::script_active_project();

extern "C" void* fjell_resolve_weak_ref(uint32_t id, uint32_t generation) {
    if (!s_active_scene || id == 0) return nullptr;
    if (s_active_scene->generation_of(id) != generation) return nullptr;
    return s_active_scene->find_spatial(id);
}

// ── Ref<T> resolution and ref counting (called from game .so) ───────────

static fjell::RefCountTable s_ref_table;

extern "C" void fjell_ref_acquire(uint32_t ref_token) {
    s_ref_table.acquire(ref_token);
}

extern "C" void fjell_ref_release(uint32_t ref_token) {
    s_ref_table.release(ref_token);
    // TODO: when count hits zero, queue resource for deferred cleanup
}

extern "C" void* fjell_ref_resolve(uint32_t /*handle_id*/) {
    // TODO: resolve handle_id through ResourceManager
    return nullptr;
}

// ── SharedRef<T> (called from game .so) ─────────────────────────────────

static fjell::RefCountTable s_shared_ref_table;

extern "C" void fjell_shared_ref_acquire(uint32_t ref_token) {
    s_shared_ref_table.acquire(ref_token);
}

extern "C" void fjell_shared_ref_release(uint32_t ref_token) {
    s_shared_ref_table.release(ref_token);
}

extern "C" void* fjell_shared_ref_resolve(uint32_t /*ref_token*/) {
    // TODO: resolve through shared object pool
    return nullptr;
}

// ── GC trace bridge functions (called from generated code in game .so) ──

static fjell::GcVisitor* s_active_visitor = nullptr;

extern "C" void fjell_gc_trace_weak(void* /*visitor*/, void* weak_ref_data) {
    if (s_active_visitor && weak_ref_data) {
        s_active_visitor->trace_weak(*static_cast<fjell::WeakRefData*>(weak_ref_data));
    }
}

extern "C" void fjell_gc_trace_ref(void* /*visitor*/, void* ref_data) {
    if (s_active_visitor && ref_data) {
        s_active_visitor->trace_ref(*static_cast<fjell::RefData*>(ref_data));
    }
}

extern "C" void fjell_gc_trace_shared(void* /*visitor*/, void* shared_ref_data) {
    if (s_active_visitor && shared_ref_data) {
        s_active_visitor->trace_shared(*static_cast<fjell::SharedRefData*>(shared_ref_data));
    }
}

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
    if (fd.ref_type_name) {
        ofd.ref_type_name = fd.ref_type_name;
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

} // namespace fjell

// ── C exports called by game .so during registration ────────────────────

// ── Generated reflection table (set by register_all.cpp before registration) ──

static const FjellGeneratedInfo* s_generated_table = nullptr;
static uint32_t s_generated_count = 0;

extern "C" void fjell_set_generated_table(const FjellGeneratedInfo* table, uint32_t count) {
    s_generated_table = table;
    s_generated_count = count;
}

extern "C" const FjellGeneratedInfo* fjell_find_generated(const char* name) {
    if (!s_generated_table || !name) return nullptr;
    for (uint32_t i = 0; i < s_generated_count; ++i) {
        if (std::strcmp(s_generated_table[i].name, name) == 0) {
            return &s_generated_table[i];
        }
    }
    return nullptr;
}

extern "C" void fjell_reg_component(FjellTypeRegistry* reg, const FjellComponentTypeOOP* desc) {
    if (!reg || !desc) return;
    fjell::OwnedScriptType owned;
    owned.name = desc->name;
    owned.type_id = fjell::ScriptManager::hash_type_name(owned.name);
    owned.create = desc->create;
    owned.trace = desc->trace;
    owned.loaded = true;
    for (uint32_t i = 0; i < desc->field_count; ++i) {
        owned.fields.push_back(fjell::copy_field(desc->fields[i]));
    }
    reg->scripts.push_back(std::move(owned));
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

void ScriptManager::set_resource_manager(ResourceManager* rm) {
    s_resource_manager = rm;
}

void ScriptManager::set_active_project(const Project* project) {
    s_active_project = project;
}

void ScriptManager::set_vfx_subsystem(VfxSubsystem* vfx) {
    script_vfx_subsystem() = vfx;
}

void ScriptManager::set_audio_subsystem(AudioSubsystem* audio) {
    script_audio_subsystem() = audio;
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
            if (name.starts_with("game_") && name.ends_with(".so") &&
                entry.path() != loaded_temp_path_) {
                fs::remove(entry.path());
            }
        }
    }

    static uint32_t s_reload_counter = 0;
    auto temp = so_path.parent_path() /
        ("game_" + std::to_string(++s_reload_counter) + ".so");
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
    call_begin_play(scene);

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
}

// ── Type lookups ────────────────────────────────────────────────────────

const OwnedScriptType* ScriptManager::find_script_type(const std::string& name) const {
    auto id = hash_type_name(name);
    auto it = script_type_map_.find(id);
    if (it == script_type_map_.end()) return nullptr;
    return &script_type_list_[it->second];
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
    world_.physics_ = physics;
    world_.delta_time = clock.delta_time();
    world_.elapsed_time = clock.elapsed();
}

// ── Prepare / Update ────────────────────────────────────────────────────

void ScriptManager::prepare(Scene& scene, const Input& input, const Clock& clock,
                            PhysicsWorld* physics) {
    s_active_scene = &scene;
    s_active_script_mgr = this;
    populate_world(input, clock, physics);
}

void ScriptManager::update(Scene& scene, const Input& input, const Clock& clock,
                           PhysicsWorld* physics) {
    FJELL_PROFILE_SCOPE_N("script_update");
    if (!is_loaded()) return;

    s_active_scene = &scene;
    s_active_script_mgr = this;
    populate_world(input, clock, physics);

    float dt = clock.delta_time();

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
}

// ── Lifecycle ───────────────────────────────────────────────────────────

void ScriptManager::call_begin_play(Scene& scene) {
    FJELL_PROFILE_SCOPE_N("script_begin_play");
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
            case FJELL_FIELD_WEAK_REF: {
                const auto* ref = static_cast<const WeakRefData*>(ptr);
                out[f.name] = {{"id", ref->id}, {"generation", ref->generation}};
                break;
            }
            case FJELL_FIELD_REF: {
                const auto* ref = static_cast<const RefData*>(ptr);
                std::string path;
                if (ref->handle_id != 0 && s_resource_manager) {
                    if (f.ref_type_name == "VfxAsset") {
                        path = s_resource_manager->asset_path_of(VfxAssetHandle{ref->handle_id});
                    } else if (f.ref_type_name == "AudioAsset") {
                        path = s_resource_manager->asset_path_of(AudioAssetHandle{ref->handle_id});
                    } else if (f.ref_type_name == "Material") {
                        path = s_resource_manager->asset_path_of(MaterialHandle{ref->handle_id});
                    }
                }
                out[f.name] = {{"asset_path", path}};
                break;
            }
            case FJELL_FIELD_SHARED_REF: {
                const auto* ref = static_cast<const SharedRefData*>(ptr);
                out[f.name] = {{"ref_token", ref->ref_token}};
                break;
            }
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
                if (j.is_object() && j.contains("asset_path") && s_resource_manager) {
                    auto path = j["asset_path"].get<std::string>();
                    if (!path.empty()) {
                        auto result = s_resource_manager->load_asset_by_type(
                            f.ref_type_name, path);
                        if (result) {
                            ref->handle_id = *result;
                        }
                    }
                }
                break;
            }
            case FJELL_FIELD_SHARED_REF: {
                auto* ref = static_cast<SharedRefData*>(ptr);
                auto& j = in[f.name];
                if (j.is_object()) {
                    ref->ref_token = j.value("ref_token", uint32_t{0});
                    if (ref->ref_token != 0) {
                        s_shared_ref_table.acquire(ref->ref_token);
                    }
                }
                break;
            }
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
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        void* ptr = base + f.offset;
        float spd = f.speed > 0.0f ? f.speed : 0.1f;

        ImGui::PushID(static_cast<int>(i));
        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                ImGui::DragFloat(f.name.c_str(), static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_INT:
                ImGui::DragInt(f.name.c_str(), static_cast<int*>(ptr),
                               spd, static_cast<int>(f.min), static_cast<int>(f.max));
                break;
            case FJELL_FIELD_BOOL:
                ImGui::Checkbox(f.name.c_str(), static_cast<bool*>(ptr));
                break;
            case FJELL_FIELD_VEC2:
                ImGui::DragFloat2(f.name.c_str(), static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_VEC3:
                ImGui::DragFloat3(f.name.c_str(), static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_VEC4:
                ImGui::DragFloat4(f.name.c_str(), static_cast<float*>(ptr), spd, f.min, f.max);
                break;
            case FJELL_FIELD_COLOR3:
                ImGui::ColorEdit3(f.name.c_str(), static_cast<float*>(ptr));
                break;
            case FJELL_FIELD_COLOR4:
                ImGui::ColorEdit4(f.name.c_str(), static_cast<float*>(ptr));
                break;
            case FJELL_FIELD_STRING:
                ImGui::InputText(f.name.c_str(), static_cast<char*>(ptr), f.size);
                break;
            case FJELL_FIELD_ENUM:
                if (!f.enum_labels.empty()) {
                    std::string preview = f.enum_labels[
                        std::clamp(*static_cast<int*>(ptr), 0,
                                   static_cast<int>(f.enum_labels.size()) - 1)];
                    if (ImGui::BeginCombo(f.name.c_str(), preview.c_str())) {
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
                if (ref->id != 0 && s_active_scene) {
                    auto* node = s_active_scene->find(ref->id);
                    if (node && s_active_scene->generation_of(ref->id) == ref->generation) {
                        target_name = node->name.c_str();
                    } else {
                        target_name = "(stale)";
                    }
                }
                ImGui::Text("%s: %s", f.name.c_str(), target_name);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    *ref = {};
                }
                break;
            }
            case FJELL_FIELD_REF: {
                auto* ref = static_cast<RefData*>(ptr);
                const auto* asset_type = find_asset_type(f.ref_type_name);

                if (asset_type && s_resource_manager) {
                    // Reverse lookup current path
                    std::string current_path;
                    if (ref->handle_id != 0) {
                        if (f.ref_type_name == "VfxAsset") {
                            current_path = s_resource_manager->asset_path_of(VfxAssetHandle{ref->handle_id});
                        } else if (f.ref_type_name == "AudioAsset") {
                            current_path = s_resource_manager->asset_path_of(AudioAssetHandle{ref->handle_id});
                        } else if (f.ref_type_name == "Material") {
                            current_path = s_resource_manager->asset_path_of(MaterialHandle{ref->handle_id});
                        }
                    }

                    auto display = current_path.empty()
                        ? std::string("(none)")
                        : std::filesystem::path(current_path).filename().string();

                    ImGui::Text("%s", f.name.c_str());
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##ref_display", display.data(), display.size(),
                                     ImGuiInputTextFlags_ReadOnly);

                    // Drag-and-drop target (same as AudioSourceComponent clip_path)
                    if (ImGui::BeginDragDropTarget()) {
                        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            std::string dropped(static_cast<const char*>(payload->Data));
                            // Only accept files matching the asset type extension
                            if (dropped.ends_with(asset_type->extension)) {
                                auto resolved = base_path::resolve_asset(
                                    dropped, s_active_project);
                                auto result = s_resource_manager->load_asset_by_type(
                                    f.ref_type_name, resolved);
                                if (result) {
                                    ref->handle_id = *result;
                                } else {
                                    FJELL_CORE_ERROR("Asset ref: {}", result.error());
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (ref->handle_id != 0) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("x")) {
                            *ref = {};
                        }
                    }
                } else {
                    ImGui::Text("%s: handle %u", f.name.c_str(), ref->handle_id);
                }
                break;
            }
            case FJELL_FIELD_SHARED_REF: {
                auto* ref = static_cast<SharedRefData*>(ptr);
                ImGui::Text("%s: token %u", f.name.c_str(), ref->ref_token);
                break;
            }
        }
        ImGui::PopID();
    }
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
