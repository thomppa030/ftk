#include "renderer/resources/resource_manager.hpp"
#include "core/log.hpp"
#include "renderer/bindless.hpp"
#include "scene/preset_asset.hpp"
#include "scripting/asset_type_registry.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/primitives.hpp"
#include "renderer/resources/texture.hpp"
#include "audio/audio_asset.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"

#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

// -- Pool implementation -----------------------------------------------------

template <typename T, typename HandleT>
HandleT ResourceManager::Pool<T, HandleT>::insert(const std::string& key,
                                                   std::unique_ptr<T> resource) {
    uint32_t index = static_cast<uint32_t>(slots.size());
    uint32_t gen = 1;
    slots.push_back(std::move(resource));
    generations.push_back(gen);
    states.push_back(LoadState::Ready);
    auto handle = HandleT::make(index, gen);
    if (!key.empty()) {
        name_to_handle.emplace(key, handle);
    }
    return handle;
}

template <typename T, typename HandleT>
T* ResourceManager::Pool<T, HandleT>::get(HandleT handle) const {
    if (!handle.valid()) return nullptr;
    uint32_t idx = handle.index();
    if (idx >= slots.size()) return nullptr;
    if (generations[idx] != handle.generation()) return nullptr;
    return slots[idx].get();
}

template <typename T, typename HandleT>
HandleT ResourceManager::Pool<T, HandleT>::reserve(const std::string& key) {
    uint32_t index = static_cast<uint32_t>(slots.size());
    uint32_t gen = 1;
    slots.push_back(nullptr);
    generations.push_back(gen);
    states.push_back(LoadState::Loading);
    auto handle = HandleT::make(index, gen);
    if (!key.empty()) {
        name_to_handle.emplace(key, handle);
    }
    return handle;
}

template <typename T, typename HandleT>
void ResourceManager::Pool<T, HandleT>::emplace(HandleT handle, std::unique_ptr<T> resource) {
    uint32_t idx = handle.index();
    if (idx < slots.size() && generations[idx] == handle.generation()) {
        slots[idx] = std::move(resource);
        states[idx] = LoadState::Ready;
    }
}

template <typename T, typename HandleT>
LoadState ResourceManager::Pool<T, HandleT>::state(HandleT handle) const {
    if (!handle.valid()) return LoadState::Failed;
    uint32_t idx = handle.index();
    if (idx >= states.size()) return LoadState::Failed;
    if (generations[idx] != handle.generation()) return LoadState::Failed;
    return states[idx];
}

// Explicit instantiations
template struct ResourceManager::Pool<Model, ModelHandle>;
template struct ResourceManager::Pool<Texture, TextureHandle>;
template struct ResourceManager::Pool<Material, MaterialHandle>;

// -- ResourceManager ---------------------------------------------------------

ResourceManager::ResourceManager(VkDevice device, VmaAllocator allocator,
                                 VkPhysicalDevice physical_device,
                                 VkCommandPool command_pool, VkQueue queue,
                                 BindlessTextures& bindless, MegaBuffer* mega_buffer,
                                 MeshletBuffer* meshlet_buffer,
                                 PipelineCache* pipeline_cache)
    : device_{device}
    , allocator_{allocator}
    , physical_device_{physical_device}
    , command_pool_{command_pool}
    , queue_{queue}
    , bindless_{bindless}
    , mega_buffer_{mega_buffer}
    , meshlet_buffer_{meshlet_buffer}
    , pipeline_cache_{pipeline_cache} {
    // Create default 1x1 textures and register them in the bindless array.
    // These occupy bindless indices 0-3.
    const std::array<uint8_t, 4> white = {255, 255, 255, 255};
    const std::array<uint8_t, 4> flat_normal = {128, 128, 255, 255};
    const std::array<uint8_t, 4> default_mr_pixels = {255, 255, 0, 255};
    const std::array<uint8_t, 4> black = {0, 0, 0, 255};

    auto register_default = [&](const std::string& name, const uint8_t* pixels) -> TextureHandle {
        auto tex = std::make_unique<Texture>(device_, allocator_, physical_device_, command_pool_, queue_,
                                             pixels, 1, 1);
        uint32_t bindless_idx = bindless_.add(*tex);
        auto handle = textures_.insert(name, std::move(tex));
        texture_to_bindless_[handle.index()] = bindless_idx;
        return handle;
    };

    default_white_handle_  = register_default("__default_white", white.data());
    default_normal_handle_ = register_default("__default_normal", flat_normal.data());
    default_mr_handle_     = register_default("__default_mr", default_mr_pixels.data());
    default_black_handle_  = register_default("__default_black", black.data());
}

Result<ModelHandle> ResourceManager::load_model(const std::string& path) {
    auto it = models_.name_to_handle.find(path);
    if (it != models_.name_to_handle.end()) {
        FJELL_GFX_DEBUG("Model cache hit: {}", path);
        return it->second;
    }

    try {
        fs::path file_path(path);
        auto ext = file_path.extension().string();

        // Direct .fjmesh load
        if (ext == ".fjmesh") {
            auto data = Model::load_fjmesh(path);
            auto model = std::make_unique<Model>(allocator_, device_, command_pool_,
                                                  queue_, std::move(data), mega_buffer_,
                                                  meshlet_buffer_);
            return models_.insert(path, std::move(model));
        }

        // For .glb/.gltf/.fbx — check for a cached .fjmesh first
        if (!cache_dir_.empty()) {
            fs::path cached = cache_dir_ / fs::path(path + ".fjmesh");
            if (fs::exists(cached)) {
                std::error_code ec;
                auto src_time = fs::last_write_time(file_path, ec);
                auto cache_time = fs::last_write_time(cached, ec);
                if (!ec && cache_time >= src_time) {
                    FJELL_GFX_DEBUG("Loading cached .fjmesh for: {}", path);
                    auto data = Model::load_fjmesh(cached.string());
                    auto model = std::make_unique<Model>(allocator_, device_, command_pool_,
                                                          queue_, std::move(data), mega_buffer_,
                                                          meshlet_buffer_);
                    return models_.insert(path, std::move(model));
                }
            }
        }

        // Fall back to glTF loading (existing path)
        auto model = std::make_unique<Model>(allocator_, device_, physical_device_, command_pool_, queue_,
                                              path, mega_buffer_, meshlet_buffer_);
        return models_.insert(path, std::move(model));
    } catch (const std::exception& e) {
        return make_error("Failed to load model '" + path + "': " + e.what());
    }
}

Result<ModelHandle> ResourceManager::create_primitive(const std::string& type) {
    std::string key = "primitive:" + type;
    auto it = models_.name_to_handle.find(key);
    if (it != models_.name_to_handle.end()) {
        return it->second;
    }

    PrimitiveMeshData data;
    if (type == "cube") {
        data = primitives::cube();
    } else if (type == "plane" || type.starts_with("plane:")) {
        uint32_t subs = 1;
        if (auto colon = type.find(':'); colon != std::string::npos) {
            subs = static_cast<uint32_t>(std::stoul(type.substr(colon + 1)));
        }
        data = primitives::plane(subs);
    } else if (type == "sphere") {
        data = primitives::sphere();
    } else {
        return make_error("Unknown primitive type: " + type);
    }

    auto model = std::make_unique<Model>(allocator_, device_, command_pool_, queue_, data, mega_buffer_, meshlet_buffer_);
    return models_.insert(key, std::move(model));
}

Result<TextureHandle> ResourceManager::load_texture(const std::string& path) {
    auto it = textures_.name_to_handle.find(path);
    if (it != textures_.name_to_handle.end()) {
        FJELL_GFX_DEBUG("Texture cache hit: {}", path);
        return it->second;
    }

    try {
        auto texture = std::make_unique<Texture>(device_, allocator_, physical_device_, command_pool_, queue_, path);
        uint32_t bindless_idx = bindless_.add(*texture);
        auto handle = textures_.insert(path, std::move(texture));
        texture_to_bindless_[handle.index()] = bindless_idx;
        return handle;
    } catch (const std::exception& e) {
        return make_error("Failed to load texture '" + path + "': " + e.what());
    }
}

Result<TextureHandle> ResourceManager::load_texture_nearest(const std::string& path) {
    // Use a distinct cache key so the same image can be loaded with different samplers
    std::string key = path + "?nearest";
    auto it = textures_.name_to_handle.find(key);
    if (it != textures_.name_to_handle.end()) {
        return it->second;
    }

    try {
        auto texture = std::make_unique<Texture>(device_, allocator_, physical_device_, command_pool_, queue_, path, true);
        uint32_t bindless_idx = bindless_.add(*texture);
        auto handle = textures_.insert(key, std::move(texture));
        texture_to_bindless_[handle.index()] = bindless_idx;
        return handle;
    } catch (const std::exception& e) {
        return make_error("Failed to load texture (nearest) '" + path + "': " + e.what());
    }
}

MaterialHandle ResourceManager::create_material(const std::string& name,
                                                 TextureHandle base_color,
                                                 TextureHandle normal_map,
                                                 TextureHandle metallic_roughness,
                                                 TextureHandle emissive,
                                                 PbrParams params) {
    auto it = materials_.name_to_handle.find(name);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    // Resolve texture handles to bindless indices (fall back to defaults)
    uint32_t bc_idx = bindless_index(base_color.valid()          ? base_color          : default_white_handle_);
    uint32_t nm_idx = bindless_index(normal_map.valid()          ? normal_map          : default_normal_handle_);
    uint32_t mr_idx = bindless_index(metallic_roughness.valid()  ? metallic_roughness  : default_mr_handle_);
    uint32_t em_idx = bindless_index(emissive.valid()            ? emissive            : default_black_handle_);

    // Build GPU material and register in SSBO
    auto material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
    uint32_t mat_id = bindless_.add_material(material->gpu_material());
    material = std::make_unique<Material>(mat_id, bc_idx, nm_idx, mr_idx, em_idx, params);

    return materials_.insert(name, std::move(material));
}

MaterialHandle ResourceManager::create_material_from_texture(const std::string& name,
                                                              Texture* base_color,
                                                              PbrParams params) {
    auto it = materials_.name_to_handle.find(name);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    // Register the model-embedded texture in bindless (idempotent)
    uint32_t bc_idx = base_color ? bindless_.add(*base_color)
                                 : bindless_index(default_white_handle_);
    uint32_t nm_idx = bindless_index(default_normal_handle_);
    uint32_t mr_idx = bindless_index(default_mr_handle_);
    uint32_t em_idx = bindless_index(default_black_handle_);

    auto material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
    uint32_t mat_id = bindless_.add_material(material->gpu_material());
    material = std::make_unique<Material>(mat_id, bc_idx, nm_idx, mr_idx, em_idx, params);

    return materials_.insert(name, std::move(material));
}

Result<MaterialHandle> ResourceManager::load_material_asset(const std::string& path,
                                                             const Project* project) {
    auto it = materials_.name_to_handle.find(path);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    auto asset = fjell::load_material_asset(path);

    // Register shader in pipeline cache
    uint32_t pipeline_id = 0;
    const ShaderMetadata* shader_meta = nullptr;
    if (pipeline_cache_) {
        pipeline_id = pipeline_cache_->register_shader(asset.shader);
        shader_meta = &pipeline_cache_->metadata(pipeline_id);
    }

    // Resolve PBR textures
    auto resolve_tex = [&](const std::string& name, TextureHandle fallback) -> TextureHandle {
        auto tex_it = asset.textures.find(name);
        if (tex_it != asset.textures.end()) {
            auto result = load_texture(base_path::resolve_asset(tex_it->second, project));
            if (result) return *result;
            FJELL_GFX_WARN("Material '{}': {}", path, result.error());
        }
        return fallback;
    };

    auto bc = resolve_tex("base_color", default_white_handle_);
    auto nm = resolve_tex("normal_map", default_normal_handle_);
    auto mr = resolve_tex("metallic_roughness", default_mr_handle_);
    auto em = resolve_tex("emissive", default_black_handle_);

    uint32_t bc_idx = bindless_index(bc);
    uint32_t nm_idx = bindless_index(nm);
    uint32_t mr_idx = bindless_index(mr);
    uint32_t em_idx = bindless_index(em);

    PbrParams params;
    params.has_normal_map = asset.textures.contains("normal_map");
    auto uv_it = asset.params.find("uv_scale");
    if (uv_it != asset.params.end() && uv_it->second.size() >= 2) {
        params.uv_scale = {uv_it->second[0], uv_it->second[1]};
    }
    auto material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
    material->set_pipeline_id(pipeline_id);
    material->set_shader_path(asset.shader);

    // Resolve custom textures and params from shader metadata
    if (shader_meta) {
        for (const auto& u : shader_meta->uniforms) {
            if (u.type != UniformType::Sampler2D) continue;
            auto tex_it = asset.textures.find(u.name);
            if (tex_it != asset.textures.end()) {
                const auto& tex_path = tex_it->second;
                if (tex_path.starts_with("$viewport:")) {
                    uint32_t idx = viewport_resolver_ ? viewport_resolver_(tex_path.substr(10)) : 0;
                    if (idx == 0) idx = bindless_index(default_white_handle_);
                    material->set_custom_texture(u.slot, idx);
                } else {
                    auto tex_result = load_texture(base_path::resolve_asset(tex_path, project));
                    auto tex_handle = tex_result ? *tex_result : default_white_handle_;
                    material->set_custom_texture(u.slot, bindless_index(tex_handle));
                }
            } else {
                TextureHandle default_tex = default_white_handle_;
                if (u.hint == UniformHint::Black) default_tex = default_black_handle_;
                else if (u.hint == UniformHint::Normal) default_tex = default_normal_handle_;
                else if (u.hint == UniformHint::DefaultMr) default_tex = default_mr_handle_;
                material->set_custom_texture(u.slot, bindless_index(default_tex));
            }
        }

        if (shader_meta->custom_param_count > 0) {
            uint32_t offset = bindless_.allocate_params(shader_meta->custom_param_count);
            material->set_custom_params(offset, shader_meta->custom_param_count);

            std::vector<float> param_data(shader_meta->custom_param_count, 0.0f);
            for (const auto& u : shader_meta->uniforms) {
                if (u.type == UniformType::Sampler2D) continue;
                uint32_t count = uniform_float_count(u.type);

                auto param_it = asset.params.find(u.name);
                if (param_it != asset.params.end()) {
                    for (uint32_t i = 0; i < count && i < param_it->second.size(); ++i) {
                        param_data[u.slot + i] = param_it->second[i];
                    }
                } else if (!u.default_value.empty()) {
                    for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                        param_data[u.slot + i] = u.default_value[i];
                    }
                }
            }
            bindless_.write_params(offset, param_data.data(), shader_meta->custom_param_count);
        }
    }

    // Compute emission color for RT GI from FJSL uniform metadata.
    // Scans resolved param values for emission/glow uniforms and combines them.
    if (shader_meta) {
        float emission_strength = 0.0f;
        glm::vec3 glow_color{0.0f};
        bool has_glow_color = false;

        for (const auto& u : shader_meta->uniforms) {
            if (u.type == UniformType::Sampler2D) continue;

            // Resolve actual value (from .fjmat or default)
            std::vector<float> values = u.default_value;
            auto p_it = asset.params.find(u.name);
            if (p_it != asset.params.end()) values = p_it->second;

            if ((u.name == "emission_strength" || u.name == "glow_strength") && !values.empty()) {
                emission_strength = values[0];
            } else if ((u.name == "glow_color" || u.name == "emission_color") && values.size() >= 3) {
                glow_color = {values[0], values[1], values[2]};
                has_glow_color = true;
            }
        }

        if (has_glow_color && emission_strength > 0.0f) {
            params.emission_color = glow_color * emission_strength;
        } else if (has_glow_color) {
            params.emission_color = glow_color;
        } else if (emission_strength > 0.0f) {
            params.emission_color = glm::vec3(emission_strength);
        }

    }

    // Rebuild material with updated emission params
    {
        uint32_t saved_param_offset = material->custom_param_offset();
        uint32_t saved_param_count = material->custom_param_count();
        material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
        material->set_pipeline_id(pipeline_id);
        material->set_shader_path(asset.shader);
        material->set_custom_params(saved_param_offset, saved_param_count);
    }

    // Register GPU material — the first material object has param_offset/count set correctly
    GpuMaterial first_gpu = material->gpu_material();
    uint32_t mat_id = bindless_.add_material(first_gpu);

    // Reconstruct with correct mat_id (Material stores it immutably in ctor)
    material = std::make_unique<Material>(mat_id, bc_idx, nm_idx, mr_idx, em_idx, params);
    material->set_pipeline_id(pipeline_id);
    material->set_shader_path(asset.shader);
    if (shader_meta) {
        if (shader_meta->render_state.force_transparent) {
            material->set_force_transparent(true);
        }
        if (!shader_meta->vertex_body.empty()) {
            material->set_aabb_padding(2.0f); // account for vertex displacement
        }
    }
    material->set_custom_params(first_gpu.param_offset, first_gpu.param_count);

    // Re-apply custom texture settings
    if (shader_meta) {
        for (const auto& u : shader_meta->uniforms) {
            if (u.type != UniformType::Sampler2D) continue;
            auto tex_it = asset.textures.find(u.name);
            if (tex_it != asset.textures.end()) {
                const auto& tex_path = tex_it->second;
                if (tex_path.starts_with("$viewport:")) {
                    std::string vp_name = tex_path.substr(10);
                    uint32_t idx = viewport_resolver_ ? viewport_resolver_(vp_name) : 0;
                    if (idx == 0) idx = bindless_index(default_white_handle_);
                    material->set_custom_texture(u.slot, idx);
                    viewport_bindings_[vp_name].push_back({mat_id, u.slot});
                } else {
                    auto tex_result = load_texture(base_path::resolve_asset(tex_path, project));
                    auto tex_handle = tex_result ? *tex_result : default_white_handle_;
                    material->set_custom_texture(u.slot, bindless_index(tex_handle));
                }
            } else {
                TextureHandle default_tex = default_white_handle_;
                if (u.hint == UniformHint::Black) default_tex = default_black_handle_;
                else if (u.hint == UniformHint::Normal) default_tex = default_normal_handle_;
                else if (u.hint == UniformHint::DefaultMr) default_tex = default_mr_handle_;
                material->set_custom_texture(u.slot, bindless_index(default_tex));
            }
        }
        // Update GPU material with final state (custom textures applied)
        bindless_.update_material(mat_id, material->gpu_material());
    }

    return materials_.insert(path, std::move(material));
}

ModelHandle ResourceManager::find_model(const std::string& path) const {
    auto it = models_.name_to_handle.find(path);
    if (it != models_.name_to_handle.end()) {
        return it->second;
    }
    return {};
}

ModelHandle ResourceManager::reserve_model(const std::string& path) {
    return models_.reserve(path);
}

void ResourceManager::emplace_model(ModelHandle handle, std::unique_ptr<Model> model) {
    models_.emplace(handle, std::move(model));
}

void ResourceManager::fail_model(ModelHandle handle) {
    uint32_t idx = handle.index();
    if (idx < models_.states.size() && models_.generations[idx] == handle.generation()) {
        models_.states[idx] = LoadState::Failed;
    }
}

LoadState ResourceManager::model_state(ModelHandle handle) const {
    return models_.state(handle);
}

void ResourceManager::set_viewport_resolver(ViewportResolver resolver) {
    viewport_resolver_ = std::move(resolver);
}

void ResourceManager::update_viewport_bindings(const std::string& viewport_name, uint32_t bindless_idx) {
    auto it = viewport_bindings_.find(viewport_name);
    if (it == viewport_bindings_.end()) return;

    for (const auto& binding : it->second) {
        auto gpu_mat = bindless_.material_at(binding.material_id);
        gpu_mat.texture_indices[binding.slot] = bindless_idx;
        bindless_.update_material(binding.material_id, gpu_mat);
    }
}

uint32_t ResourceManager::bindless_index(TextureHandle handle) const {
    auto it = texture_to_bindless_.find(handle.index());
    if (it != texture_to_bindless_.end()) {
        return it->second;
    }
    return 0; // fallback to default white
}

Model* ResourceManager::get(ModelHandle handle) const {
    return models_.get(handle);
}

// ── Skeletal models ─────────────────────────────────────────────────────

Result<SkeletalModelHandle> ResourceManager::load_skeletal_model(const std::string& path) {
    auto it = skeletal_models_.name_to_handle.find(path);
    if (it != skeletal_models_.name_to_handle.end()) {
        return it->second;
    }

    try {
        auto data = SkeletalModel::load_fjskel(path);
        auto model = std::make_unique<SkeletalModel>(allocator_, device_, command_pool_,
                                                      queue_, std::move(data));
        return skeletal_models_.insert(path, std::move(model));
    } catch (const std::exception& e) {
        return make_error("Failed to load skeletal model '" + path + "': " + e.what());
    }
}

SkeletalModelHandle ResourceManager::find_skeletal_model(const std::string& path) const {
    auto it = skeletal_models_.name_to_handle.find(path);
    return it != skeletal_models_.name_to_handle.end() ? it->second : SkeletalModelHandle{};
}

SkeletalModelHandle ResourceManager::reserve_skeletal_model(const std::string& path) {
    return skeletal_models_.reserve(path);
}

void ResourceManager::emplace_skeletal_model(SkeletalModelHandle handle,
                                              std::unique_ptr<SkeletalModel> model) {
    skeletal_models_.emplace(handle, std::move(model));
}

void ResourceManager::fail_skeletal_model(SkeletalModelHandle handle) {
    uint32_t idx = handle.index();
    if (idx < skeletal_models_.states.size() &&
        skeletal_models_.generations[idx] == handle.generation()) {
        skeletal_models_.states[idx] = LoadState::Failed;
    }
}

LoadState ResourceManager::skeletal_model_state(SkeletalModelHandle handle) const {
    return skeletal_models_.state(handle);
}

SkeletalModel* ResourceManager::get(SkeletalModelHandle handle) const {
    return skeletal_models_.get(handle);
}

Texture* ResourceManager::get(TextureHandle handle) const {
    return textures_.get(handle);
}

Material* ResourceManager::get(MaterialHandle handle) const {
    return materials_.get(handle);
}

MaterialHandle ResourceManager::find_material(const std::string& name) const {
    auto it = materials_.name_to_handle.find(name);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }
    return {};
}

// ── VFX assets ───────────────────────────────────────────────────────────

Result<VfxAssetHandle> ResourceManager::load_vfx_asset(const std::string& path) {
    auto it = vfx_assets_.name_to_handle.find(path);
    if (it != vfx_assets_.name_to_handle.end()) {
        return it->second;
    }

    try {
        auto effect = std::make_unique<VfxEffect>(load_vfx_effect(path));
        return vfx_assets_.insert(path, std::move(effect));
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
}

VfxEffect* ResourceManager::get(VfxAssetHandle handle) const {
    return vfx_assets_.get(handle);
}

// ── Audio assets ─────────────────────────────────────────────────────────

Result<AudioAssetHandle> ResourceManager::load_audio_asset(const std::string& path) {
    auto it = audio_assets_.name_to_handle.find(path);
    if (it != audio_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto result = load_audio_asset_file(path);
    if (!result) return make_error(result.error());
    auto asset = std::make_unique<AudioAsset>(std::move(*result));
    return audio_assets_.insert(path, std::move(asset));
}

AudioAsset* ResourceManager::get(AudioAssetHandle handle) const {
    return audio_assets_.get(handle);
}

// ── Input action assets ──────────────────────────────────────────────────

Result<InputActionHandle> ResourceManager::load_input_action(const std::string& path) {
    auto it = input_actions_.name_to_handle.find(path);
    if (it != input_actions_.name_to_handle.end()) {
        return it->second;
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Cannot open " + path);
    }

    auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) {
        return make_error("Invalid JSON in " + path);
    }

    auto def = std::make_unique<InputActionDef>();
    def->name = j.value("name", "");
    auto type_str = j.value("type", "button");
    if (type_str == "axis1d") def->type = ActionType::axis1d;
    else if (type_str == "axis2d") def->type = ActionType::axis2d;
    else def->type = ActionType::button;

    return input_actions_.insert(path, std::move(def));
}

InputActionDef* ResourceManager::get(InputActionHandle handle) const {
    return input_actions_.get(handle);
}

// ── Generic asset loading ────────────────────────────────────────────────

Result<uint32_t> ResourceManager::load_asset_by_type(const std::string& type_name,
                                                      const std::string& path) {
    auto* entry = AssetTypeRegistry::instance().find_by_name(type_name);
    if (!entry || !entry->load) {
        return make_error("Unknown asset type: " + type_name);
    }
    return entry->load(*this, path);
}

std::string ResourceManager::asset_path_of(VfxAssetHandle handle) const {
    return vfx_assets_.path_of(handle);
}

std::string ResourceManager::asset_path_of(AudioAssetHandle handle) const {
    return audio_assets_.path_of(handle);
}

std::string ResourceManager::asset_path_of(MaterialHandle handle) const {
    return materials_.path_of(handle);
}

std::string ResourceManager::asset_path_of(InputActionHandle handle) const {
    return input_actions_.path_of(handle);
}

// ── Preset assets ────────────────────────────────────────────────────────

Result<PresetAssetHandle> ResourceManager::load_preset_asset(const std::string& path) {
    auto it = preset_assets_.name_to_handle.find(path);
    if (it != preset_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto result = Preset::load(path, *this);
    if (!result) {
        return make_error(result.error());
    }

    auto asset = std::make_unique<PresetAsset>();
    asset->preset = std::move(*result);
    return preset_assets_.insert(path, std::move(asset));
}

PresetAsset* ResourceManager::get(PresetAssetHandle handle) const {
    return preset_assets_.get(handle);
}

std::string ResourceManager::asset_path_of(PresetAssetHandle handle) const {
    return preset_assets_.path_of(handle);
}

} // namespace fjell
