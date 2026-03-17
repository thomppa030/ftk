#include "scripting/script_manager.hpp"
#include "scripting/fjell_actor.hpp"
#include "scripting/fjell_component.hpp"
#include "scripting/json_bridge.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "physics/physics_world.hpp"
#include "scene/scene.hpp"

#include <imgui.h>

#include <dlfcn.h>

#ifndef NDEBUG
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <functional>

// ── Type registry (engine-side, filled by game .so) ─────────────────────

struct FjellTypeRegistry {
    std::vector<fjell::OwnedActorType> actors;
    std::vector<fjell::OwnedOOPComponentType> components;
};

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
    if (fd.enum_labels) {
        for (uint32_t e = 0; e < fd.size; ++e) {
            ofd.enum_labels.emplace_back(fd.enum_labels[e]);
        }
    }
    return ofd;
}

} // namespace fjell

// ── C exports called by game .so during registration ────────────────────

extern "C" void fjell_reg_actor(FjellTypeRegistry* reg, const FjellActorType* desc) {
    if (!reg || !desc) return;
    fjell::OwnedActorType owned;
    owned.name = desc->name;
    owned.type_id = fjell::ScriptManager::hash_type_name(owned.name);
    owned.create = desc->create;
    owned.loaded = true;
    for (uint32_t i = 0; i < desc->field_count; ++i) {
        owned.fields.push_back(fjell::copy_field(desc->fields[i]));
    }
    reg->actors.push_back(std::move(owned));
}

extern "C" void fjell_reg_component(FjellTypeRegistry* reg, const FjellComponentTypeOOP* desc) {
    if (!reg || !desc) return;
    fjell::OwnedOOPComponentType owned;
    owned.name = desc->name;
    owned.type_id = fjell::ScriptManager::hash_type_name(owned.name);
    owned.create = desc->create;
    owned.loaded = true;
    for (uint32_t i = 0; i < desc->field_count; ++i) {
        owned.fields.push_back(fjell::copy_field(desc->fields[i]));
    }
    reg->components.push_back(std::move(owned));
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

ScriptManager::ScriptManager() {
#ifndef NDEBUG
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
#endif
}

ScriptManager::~ScriptManager() {
    if (handle_) {
        dlclose(handle_);
    }
    if (!loaded_temp_path_.empty()) {
        std::filesystem::remove(loaded_temp_path_);
    }
#ifndef NDEBUG
    if (watch_fd_ >= 0) {
        inotify_rm_watch(inotify_fd_, watch_fd_);
    }
    if (inotify_fd_ >= 0) {
        close(inotify_fd_);
    }
#endif
}

// ── Load / Unload / Reload ──────────────────────────────────────────────

bool ScriptManager::load(const std::filesystem::path& so_path) {
    if (!std::filesystem::exists(so_path)) {
        FJELL_CORE_WARN("Script library not found: {}", so_path.string());
        return false;
    }

    auto temp = so_path.parent_path() /
        ("game_" + std::to_string(std::time(nullptr)) + ".so");
    std::filesystem::copy_file(so_path, temp,
                               std::filesystem::copy_options::overwrite_existing);

    void* h = dlopen(temp.c_str(), RTLD_NOW);
    if (!h) {
        FJELL_CORE_ERROR("dlopen failed: {}", dlerror());
        std::filesystem::remove(temp);
        return false;
    }

    handle_ = h;
    so_path_ = so_path;

    if (!loaded_temp_path_.empty()) {
        std::filesystem::remove(loaded_temp_path_);
    }
    loaded_temp_path_ = temp;

    build_registry();

    int total = static_cast<int>(type_list_.size() +
                                  actor_type_list_.size() +
                                  oop_type_list_.size());
    FJELL_CORE_INFO("Scripts loaded: {} type(s)", total);
    return true;
}

void ScriptManager::unload(Scene& scene) {
    if (!handle_) return;

    serialize_all(scene);

    // Destroy old-style component instances
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.components) {
            const auto* type = find_type(comp.type_id);
            if (type && type->loaded && type->destroy && !comp.data.empty()) {
                type->destroy(comp.data.data());
            }
            comp.data.clear();
        }
    }

    // Destroy OOP component and actor instances
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.oop_components) {
            comp.instance.reset();
        }
        obj.actor.reset();
    }

    dlclose(handle_);
    handle_ = nullptr;

    for (auto& t : type_list_) t.loaded = false;
    for (auto& t : actor_type_list_) t.loaded = false;
    for (auto& t : oop_type_list_) t.loaded = false;
}

bool ScriptManager::reload(Scene& scene, const std::filesystem::path& so_path) {
    FJELL_CORE_INFO("Hot-reloading scripts...");
    unload(scene);

    if (!load(so_path)) {
        FJELL_CORE_ERROR("Reload failed — scripts unloaded");
        return false;
    }

    deserialize_all(scene);
    FJELL_CORE_INFO("Scripts reloaded");
    return true;
}

// ── Registry building ───────────────────────────────────────────────────

void ScriptManager::build_registry() {
    type_list_.clear();
    type_map_.clear();
    actor_type_list_.clear();
    actor_type_map_.clear();
    oop_type_list_.clear();
    oop_type_map_.clear();

    // Try new-style registration first (fjell_register)
    using NewRegFn = void (*)(FjellTypeRegistry*);
    auto* new_reg = reinterpret_cast<NewRegFn>(dlsym(handle_, "fjell_register"));
    if (new_reg) {
        FjellTypeRegistry registry;
        new_reg(&registry);

        for (auto& at : registry.actors) {
            actor_type_map_[at.type_id] = actor_type_list_.size();
            actor_type_list_.push_back(std::move(at));
        }
        for (auto& ct : registry.components) {
            oop_type_map_[ct.type_id] = oop_type_list_.size();
            oop_type_list_.push_back(std::move(ct));
        }
    }

    // Also try old-style registration (fjell_register_components)
    auto* old_reg = reinterpret_cast<FjellRegisterFn>(
        dlsym(handle_, "fjell_register_components"));
    if (old_reg) {
        const FjellComponentType* const* types = old_reg();
        if (types) {
            for (int i = 0; types[i] != nullptr; ++i) {
                const auto* t = types[i];
                OwnedComponentType owned;
                owned.name = t->name;
                owned.type_id = hash_type_name(owned.name);
                owned.instance_size = t->instance_size;
                owned.version = t->version;
                owned.create = t->create;
                owned.destroy = t->destroy;
                owned.update = t->update;
                owned.inspector = t->inspector;
                owned.serialize = t->serialize;
                owned.deserialize = t->deserialize;
                owned.loaded = true;

                for (uint32_t f = 0; f < t->field_count; ++f) {
                    owned.fields.push_back(copy_field(t->fields[f]));
                }

                type_map_[owned.type_id] = type_list_.size();
                type_list_.push_back(std::move(owned));
            }
        }
    }
}

// ── Type lookups ────────────────────────────────────────────────────────

const OwnedComponentType* ScriptManager::find_type(uint32_t type_id) const {
    auto it = type_map_.find(type_id);
    if (it == type_map_.end()) return nullptr;
    return &type_list_[it->second];
}

const OwnedComponentType* ScriptManager::find_type(const std::string& name) const {
    return find_type(hash_type_name(name));
}

const OwnedActorType* ScriptManager::find_actor_type(const std::string& name) const {
    auto id = hash_type_name(name);
    auto it = actor_type_map_.find(id);
    if (it == actor_type_map_.end()) return nullptr;
    return &actor_type_list_[it->second];
}

const OwnedOOPComponentType* ScriptManager::find_oop_component_type(const std::string& name) const {
    auto id = hash_type_name(name);
    auto it = oop_type_map_.find(id);
    if (it == oop_type_map_.end()) return nullptr;
    return &oop_type_list_[it->second];
}

// ── Old-style component management ──────────────────────────────────────

bool ScriptManager::add_component(RenderObject& obj, const std::string& type_name) {
    const auto* type = find_type(type_name);
    if (!type || !type->loaded) return false;

    ComponentInstance comp;
    comp.type_id = type->type_id;
    comp.type_name = type->name;
    comp.data.resize(type->instance_size, 0);

    if (type->create) {
        type->create(comp.data.data(), &api_);
    }

    obj.components.push_back(std::move(comp));
    return true;
}

void ScriptManager::remove_component(RenderObject& obj, uint32_t type_id) {
    auto it = std::find_if(obj.components.begin(), obj.components.end(),
                           [type_id](const ComponentInstance& c) { return c.type_id == type_id; });
    if (it == obj.components.end()) return;

    const auto* type = find_type(type_id);
    if (type && type->loaded && type->destroy && !it->data.empty()) {
        type->destroy(it->data.data());
    }
    obj.components.erase(it);
}

// ── New OOP component management ────────────────────────────────────────

bool ScriptManager::add_oop_component(RenderObject& obj, const std::string& type_name) {
    const auto* type = find_oop_component_type(type_name);
    if (!type || !type->loaded || !type->create) return false;

    ComponentEntry entry;
    entry.type_name = type->name;
    entry.instance.reset(type->create());
    entry.instance->owner_ = obj.actor.get();

    obj.oop_components.push_back(std::move(entry));
    return true;
}

void ScriptManager::remove_oop_component(RenderObject& obj, const std::string& type_name) {
    auto it = std::find_if(obj.oop_components.begin(), obj.oop_components.end(),
                           [&type_name](const ComponentEntry& c) { return c.type_name == type_name; });
    if (it == obj.oop_components.end()) return;
    obj.oop_components.erase(it);
}

// ── Engine API callbacks (old-style) ────────────────────────────────────

static ScriptManager* s_active_manager = nullptr;
static PhysicsWorld* s_active_physics = nullptr;

void ScriptManager::populate_engine_api(Scene& scene, const Input& input, const Clock& clock,
                                        PhysicsWorld* physics) {
    current_scene_ = &scene;
    s_active_manager = this;
    s_active_physics = physics;

    // World struct for OOP actors
    world_.input_ = &input;
    world_.physics_ = physics;
    world_.scene_ = &scene;
    world_.delta_time = clock.delta_time();

    api_.delta_time = clock.delta_time();

    api_.get_position = [](uint32_t id, float* out) {
        auto* obj = s_active_manager->current_scene_->find(id);
        if (obj) { std::memcpy(out, &obj->transform.position, sizeof(float) * 3); }
    };
    api_.set_position = [](uint32_t id, const float* in) {
        auto* obj = s_active_manager->current_scene_->find(id);
        if (obj) { std::memcpy(&obj->transform.position, in, sizeof(float) * 3); }
    };
    api_.get_rotation = [](uint32_t id, float* out) {
        auto* obj = s_active_manager->current_scene_->find(id);
        if (obj) { std::memcpy(out, &obj->transform.rotation, sizeof(float) * 3); }
    };
    api_.set_rotation = [](uint32_t id, const float* in) {
        auto* obj = s_active_manager->current_scene_->find(id);
        if (obj) { std::memcpy(&obj->transform.rotation, in, sizeof(float) * 3); }
    };
    api_.get_scale = [](uint32_t id, float* out) {
        auto* obj = s_active_manager->current_scene_->find(id);
        if (obj) { std::memcpy(out, &obj->transform.scale, sizeof(float) * 3); }
    };
    api_.set_scale = [](uint32_t id, const float* in) {
        auto* obj = s_active_manager->current_scene_->find(id);
        if (obj) { std::memcpy(&obj->transform.scale, in, sizeof(float) * 3); }
    };

    static const Input* s_input = nullptr;
    s_input = &input;

    api_.is_key_held = [](int key) -> uint8_t {
        return s_input->is_key_held(key) ? 1 : 0;
    };
    api_.is_key_pressed = [](int key) -> uint8_t {
        return s_input->is_key_pressed(key) ? 1 : 0;
    };
    api_.is_key_released = [](int key) -> uint8_t {
        return s_input->is_key_released(key) ? 1 : 0;
    };
    api_.is_mouse_button_held = [](int btn) -> uint8_t {
        return s_input->is_mouse_button_held(btn) ? 1 : 0;
    };
    api_.is_mouse_button_pressed = [](int btn) -> uint8_t {
        return s_input->is_mouse_button_pressed(btn) ? 1 : 0;
    };
    api_.get_mouse_delta = [](float* out) {
        auto d = s_input->mouse_delta();
        out[0] = static_cast<float>(d.x);
        out[1] = static_cast<float>(d.y);
    };

    api_.get_component = [](uint32_t object_id, const char* type_name) -> void* {
        auto* obj = s_active_manager->current_scene_->find(object_id);
        if (!obj) return nullptr;
        uint32_t tid = ScriptManager::hash_type_name(type_name);
        for (auto& comp : obj->components) {
            if (comp.type_id == tid && !comp.data.empty()) return comp.data.data();
        }
        return nullptr;
    };

    api_.log_info = [](const char* msg) { FJELL_CORE_INFO("[script] {}", msg); };
    api_.log_warn = [](const char* msg) { FJELL_CORE_WARN("[script] {}", msg); };
    api_.log_error = [](const char* msg) { FJELL_CORE_ERROR("[script] {}", msg); };

    // Physics
    api_.apply_force = [](uint32_t id, const float* xyz) {
        if (s_active_physics) s_active_physics->apply_force(id, {xyz[0], xyz[1], xyz[2]});
    };
    api_.apply_impulse = [](uint32_t id, const float* xyz) {
        if (s_active_physics) s_active_physics->apply_impulse(id, {xyz[0], xyz[1], xyz[2]});
    };
    api_.set_linear_velocity = [](uint32_t id, const float* xyz) {
        if (s_active_physics) s_active_physics->set_linear_velocity(id, {xyz[0], xyz[1], xyz[2]});
    };
    api_.get_linear_velocity = [](uint32_t id, float* out) {
        if (s_active_physics) {
            auto v = s_active_physics->get_linear_velocity(id);
            out[0] = v.x; out[1] = v.y; out[2] = v.z;
        }
    };
    api_.set_angular_velocity = [](uint32_t id, const float* xyz) {
        if (s_active_physics) s_active_physics->set_angular_velocity(id, {xyz[0], xyz[1], xyz[2]});
    };
    api_.get_angular_velocity = [](uint32_t id, float* out) {
        if (s_active_physics) {
            auto v = s_active_physics->get_angular_velocity(id);
            out[0] = v.x; out[1] = v.y; out[2] = v.z;
        }
    };
    api_.raycast = [](const float* origin, const float* direction,
                      float max_dist, float* out_pos, float* out_normal,
                      float* out_dist, uint32_t* out_id) -> uint8_t {
        if (!s_active_physics) return 0;
        RaycastHit hit;
        bool ok = s_active_physics->raycast(
            {origin[0], origin[1], origin[2]},
            {direction[0], direction[1], direction[2]},
            max_dist, hit);
        if (ok) {
            if (out_pos) { out_pos[0] = hit.position.x; out_pos[1] = hit.position.y; out_pos[2] = hit.position.z; }
            if (out_normal) { out_normal[0] = hit.normal.x; out_normal[1] = hit.normal.y; out_normal[2] = hit.normal.z; }
            if (out_dist) *out_dist = hit.distance;
            if (out_id) *out_id = hit.object_id;
        }
        return ok ? 1 : 0;
    };
}

// ── Update ──────────────────────────────────────────────────────────────

void ScriptManager::update(Scene& scene, const Input& input, const Clock& clock,
                           PhysicsWorld* physics) {
    if (!is_loaded()) return;

    populate_engine_api(scene, input, clock, physics);

    float dt = clock.delta_time();

    // Old-style C API components
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.components) {
            const auto* type = find_type(comp.type_id);
            if (!type || !type->loaded || !type->update || comp.data.empty()) continue;

            api_.object_id = obj.id;
            type->update(comp.data.data(), &api_, dt);
        }
    }

    // OOP actors and components
    for (auto& obj : scene.objects()) {
        if (obj.actor) {
            obj.actor->tick(dt);
        }
        for (auto& comp : obj.oop_components) {
            if (comp.instance) {
                comp.instance->tick(dt);
            }
        }
    }

    current_scene_ = nullptr;
    s_active_manager = nullptr;
    s_active_physics = nullptr;
}

// ── Lifecycle ───────────────────────────────────────────────────────────

void ScriptManager::call_begin_play(Scene& scene) {
    for (auto& obj : scene.objects()) {
        if (obj.actor) {
            obj.actor->world_ = &world_;
            obj.actor->begin_play();
        }
        for (auto& comp : obj.oop_components) {
            if (comp.instance) {
                comp.instance->begin_play();
            }
        }
    }
}

void ScriptManager::call_end_play(Scene& scene) {
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.oop_components) {
            if (comp.instance) {
                comp.instance->end_play();
            }
        }
        if (obj.actor) {
            obj.actor->end_play();
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
            case FJELL_FIELD_ENUM:
                *static_cast<int32_t*>(ptr) = in[f.name].get<int32_t>();
                break;
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
        }
    }
}

void ScriptManager::auto_serialize(const OwnedComponentType& type, const void* data,
                                    nlohmann::json& out) const {
    auto_serialize_fields(type.fields, data, out);
}

void ScriptManager::auto_deserialize(const OwnedComponentType& type, void* data,
                                      const nlohmann::json& in) const {
    auto_deserialize_fields(type.fields, data, in);
}

// ── Serialize / Deserialize ─────────────────────────────────────────────

void ScriptManager::serialize_all(Scene& scene) {
    for (auto& obj : scene.objects()) {
        // Old-style components
        for (auto& comp : obj.components) {
            const auto* type = find_type(comp.type_id);
            if (!type || comp.data.empty()) continue;

            comp.serialized = nlohmann::json::object();
            if (type->loaded && type->serialize) {
                JsonWriterImpl writer{&comp.serialized};
                type->serialize(comp.data.data(),
                                reinterpret_cast<FjellJsonWriter*>(&writer));
            } else {
                auto_serialize(*type, comp.data.data(), comp.serialized);
            }
        }

        // OOP components
        for (auto& comp : obj.oop_components) {
            if (!comp.instance) continue;
            const auto* type = find_oop_component_type(comp.type_name);
            if (!type) continue;

            comp.serialized = nlohmann::json::object();
            auto_serialize_fields(type->fields, comp.instance.get(), comp.serialized);
        }
    }
}

void ScriptManager::deserialize_all(Scene& scene) {
    for (auto& obj : scene.objects()) {
        // Old-style components
        for (auto& comp : obj.components) {
            if (comp.type_id == 0 && !comp.type_name.empty()) {
                comp.type_id = hash_type_name(comp.type_name);
            }

            const auto* type = find_type(comp.type_id);
            if (!type || !type->loaded) continue;

            comp.data.assign(type->instance_size, 0);

            if (type->create) {
                type->create(comp.data.data(), &api_);
            }

            if (!comp.serialized.empty()) {
                if (type->deserialize) {
                    JsonReaderImpl reader{&comp.serialized};
                    type->deserialize(comp.data.data(),
                                      reinterpret_cast<const FjellJsonReader*>(&reader));
                } else {
                    auto_deserialize(*type, comp.data.data(), comp.serialized);
                }
            }
        }

        // OOP actor
        if (!obj.actor_type_name.empty()) {
            const auto* atype = find_actor_type(obj.actor_type_name);
            if (atype && atype->loaded && atype->create) {
                obj.actor.reset(atype->create());
                obj.actor->id_ = obj.id;
                obj.actor->parent_id_ = obj.parent_id;
                obj.actor->name_ = obj.name.c_str();
                obj.actor->world_ = &world_;
                obj.actor->transform_ = &obj.transform;
            }
        }

        // OOP components
        for (auto& comp : obj.oop_components) {
            const auto* ctype = find_oop_component_type(comp.type_name);
            if (!ctype || !ctype->loaded || !ctype->create) continue;

            comp.instance.reset(ctype->create());
            comp.instance->owner_ = obj.actor.get();

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
        }
        ImGui::PopID();
    }
}

void ScriptManager::draw_inspector(RenderObject& obj) {
    uint32_t remove_id = 0;

    // Old-style components
    for (auto& comp : obj.components) {
        const auto* type = find_type(comp.type_id);
        const char* label = type ? type->name.c_str() : comp.type_name.c_str();

        ImGui::PushID(static_cast<int>(comp.type_id));
        bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove")) {
                remove_id = comp.type_id;
            }
            ImGui::EndPopup();
        }

        if (open && type && type->loaded) {
            if (type->inspector && !comp.data.empty()) {
                type->inspector(comp.data.data());
            } else if (!comp.data.empty()) {
                draw_fields_inspector(type->fields, comp.data.data());
            }
        } else if (open && (!type || !type->loaded)) {
            ImGui::TextDisabled("Script not loaded");
        }
        ImGui::PopID();
    }

    if (remove_id != 0) {
        remove_component(obj, remove_id);
    }
}

void ScriptManager::draw_oop_inspector(RenderObject& obj) {
    // Actor fields
    if (obj.actor && !obj.actor_type_name.empty()) {
        const auto* atype = find_actor_type(obj.actor_type_name);
        if (atype && atype->loaded && !atype->fields.empty()) {
            if (ImGui::CollapsingHeader(atype->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                draw_fields_inspector(atype->fields, obj.actor.get());
            }
        }
    }

    // OOP component fields
    std::string remove_name;
    for (auto& comp : obj.oop_components) {
        const auto* ctype = find_oop_component_type(comp.type_name);
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
        remove_oop_component(obj, remove_name);
    }
}

// ── Hot-reload watcher (debug only) ─────────────────────────────────────

#ifndef NDEBUG

void ScriptManager::watch(const std::filesystem::path& so_path) {
    if (inotify_fd_ < 0) return;

    if (watch_fd_ >= 0) {
        inotify_rm_watch(inotify_fd_, watch_fd_);
        watch_fd_ = -1;
    }

    auto dir = so_path.parent_path().string();
    watch_fd_ = inotify_add_watch(inotify_fd_, dir.c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO);
    if (watch_fd_ < 0) {
        FJELL_CORE_WARN("Failed to watch script directory: {}", dir);
    } else {
        FJELL_CORE_INFO("Script hot-reload watching: {}", dir);
    }
    so_path_ = so_path;
}

bool ScriptManager::poll_reload(Scene& scene) {
    if (inotify_fd_ < 0 || so_path_.empty()) return false;

    pollfd pfd{};
    pfd.fd = inotify_fd_;
    pfd.events = POLLIN;

    if (::poll(&pfd, 1, 0) <= 0) return false;

    bool changed = false;
    alignas(inotify_event) std::array<char, 4096> buf{};
    auto len = read(inotify_fd_, buf.data(), buf.size());
    if (len <= 0) return false;

    const char* ptr = buf.data();
    while (ptr < buf.data() + len) {
        const auto* event = reinterpret_cast<const inotify_event*>(ptr);
        if (event->len > 0) {
            std::string filename{event->name};
            if (filename.ends_with(".so")) {
                changed = true;
            }
        }
        ptr += sizeof(inotify_event) + event->len;
    }

    if (changed) {
        return reload(scene, so_path_);
    }
    return false;
}

#endif // NDEBUG

} // namespace fjell
