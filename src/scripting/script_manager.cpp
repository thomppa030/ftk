#include "scripting/script_manager.hpp"
#include "scripting/json_bridge.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
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

namespace fjell {

// FNV-1a hash
uint32_t ScriptManager::hash_type_name(const std::string& name) {
    uint32_t hash = 2166136261u;
    for (char c : name) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

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

bool ScriptManager::load(const std::filesystem::path& so_path) {
    if (!std::filesystem::exists(so_path)) {
        FJELL_CORE_WARN("Script library not found: {}", so_path.string());
        return false;
    }

    // Copy to temp path to avoid dlopen caching
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
    FJELL_CORE_INFO("Scripts loaded: {} component type(s)", type_list_.size());
    return true;
}

void ScriptManager::unload(Scene& scene) {
    if (!handle_) return;

    serialize_all(scene);

    // Destroy all instances
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.components) {
            const auto* type = find_type(comp.type_id);
            if (type && type->loaded && type->destroy && !comp.data.empty()) {
                type->destroy(comp.data.data());
            }
            comp.data.clear();
        }
    }

    dlclose(handle_);
    handle_ = nullptr;

    for (auto& t : type_list_) {
        t.loaded = false;
    }
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

void ScriptManager::build_registry() {
    auto* reg_fn = reinterpret_cast<FjellRegisterFn>(dlsym(handle_, "fjell_register_components"));
    if (!reg_fn) {
        FJELL_CORE_WARN("No fjell_register_components symbol found");
        return;
    }

    const FjellComponentType* const* types = reg_fn();
    if (!types) return;

    type_list_.clear();
    type_map_.clear();

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
            const auto& fd = t->fields[f];
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
            owned.fields.push_back(std::move(ofd));
        }

        type_map_[owned.type_id] = type_list_.size();
        type_list_.push_back(std::move(owned));
    }
}

const OwnedComponentType* ScriptManager::find_type(uint32_t type_id) const {
    auto it = type_map_.find(type_id);
    if (it == type_map_.end()) return nullptr;
    return &type_list_[it->second];
}

const OwnedComponentType* ScriptManager::find_type(const std::string& name) const {
    return find_type(hash_type_name(name));
}

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

// ── Engine API callbacks ────────────────────────────────────────────────

static ScriptManager* s_active_manager = nullptr;

void ScriptManager::populate_engine_api(Scene& scene, const Input& input, const Clock& clock) {
    current_scene_ = &scene;
    s_active_manager = this;

    api_.delta_time = clock.delta_time();
    // elapsed_time would need a cumulative timer — leave as 0 for now

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

    api_.log_info = [](const char* msg) { FJELL_CORE_INFO("[script] {}", msg); };
    api_.log_warn = [](const char* msg) { FJELL_CORE_WARN("[script] {}", msg); };
    api_.log_error = [](const char* msg) { FJELL_CORE_ERROR("[script] {}", msg); };
}

void ScriptManager::update(Scene& scene, const Input& input, const Clock& clock) {
    if (!is_loaded()) return;

    populate_engine_api(scene, input, clock);

    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.components) {
            const auto* type = find_type(comp.type_id);
            if (!type || !type->loaded || !type->update || comp.data.empty()) continue;

            api_.object_id = obj.id;
            type->update(comp.data.data(), &api_, clock.delta_time());
        }
    }

    current_scene_ = nullptr;
    s_active_manager = nullptr;
}

// ── Auto-serialization from field descriptors ───────────────────────────

void ScriptManager::auto_serialize(const OwnedComponentType& type, const void* data,
                                    nlohmann::json& out) const {
    const auto* base = static_cast<const uint8_t*>(data);
    for (const auto& f : type.fields) {
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

void ScriptManager::auto_deserialize(const OwnedComponentType& type, void* data,
                                      const nlohmann::json& in) const {
    auto* base = static_cast<uint8_t*>(data);
    for (const auto& f : type.fields) {
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

void ScriptManager::serialize_all(Scene& scene) {
    for (auto& obj : scene.objects()) {
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
    }
}

void ScriptManager::deserialize_all(Scene& scene) {
    for (auto& obj : scene.objects()) {
        for (auto& comp : obj.components) {
            // Resolve type_id from name if not set (loaded from JSON)
            if (comp.type_id == 0 && !comp.type_name.empty()) {
                comp.type_id = hash_type_name(comp.type_name);
            }

            const auto* type = find_type(comp.type_id);
            if (!type || !type->loaded) continue;

            // Allocate fresh data blob
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
    }
}

// ── Inspector ───────────────────────────────────────────────────────────

void ScriptManager::draw_inspector(RenderObject& obj) {
    uint32_t remove_id = 0;

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
                // Auto-inspector from field descriptors
                auto* base = comp.data.data();
                for (std::size_t i = 0; i < type->fields.size(); ++i) {
                    const auto& f = type->fields[i];
                    void* ptr = base + f.offset;
                    float spd = f.speed > 0.0f ? f.speed : 0.1f;

                    ImGui::PushID(static_cast<int>(i));
                    switch (f.type) {
                        case FJELL_FIELD_FLOAT:
                            ImGui::DragFloat(f.name.c_str(), static_cast<float*>(ptr),
                                             spd, f.min, f.max);
                            break;
                        case FJELL_FIELD_INT:
                            ImGui::DragInt(f.name.c_str(), static_cast<int*>(ptr),
                                           spd, static_cast<int>(f.min), static_cast<int>(f.max));
                            break;
                        case FJELL_FIELD_BOOL:
                            ImGui::Checkbox(f.name.c_str(), static_cast<bool*>(ptr));
                            break;
                        case FJELL_FIELD_VEC2:
                            ImGui::DragFloat2(f.name.c_str(), static_cast<float*>(ptr),
                                              spd, f.min, f.max);
                            break;
                        case FJELL_FIELD_VEC3:
                            ImGui::DragFloat3(f.name.c_str(), static_cast<float*>(ptr),
                                              spd, f.min, f.max);
                            break;
                        case FJELL_FIELD_VEC4:
                            ImGui::DragFloat4(f.name.c_str(), static_cast<float*>(ptr),
                                              spd, f.min, f.max);
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
                            // Build labels array for combo
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
        } else if (open && (!type || !type->loaded)) {
            ImGui::TextDisabled("Script not loaded");
        }
        ImGui::PopID();
    }

    if (remove_id != 0) {
        remove_component(obj, remove_id);
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
