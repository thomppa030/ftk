#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/material_gi_params.hpp"
#include "core/log.hpp"
#include "renderer/bindless.hpp"
#include "scene/preset_asset.hpp"
#include "core/asset_type_registry.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/resources/lut_asset.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/primitives.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "audio/audio_asset.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"

#include <algorithm>
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
                                 UploadContext& upload,
                                 BindlessTextures& bindless, MegaBuffer* mega_buffer,
                                 MeshletBuffer* meshlet_buffer,
                                 PipelineCache* pipeline_cache)
    : device_{device}
    , allocator_{allocator}
    , physical_device_{physical_device}
    , upload_{upload}
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
        auto tex = std::make_unique<Texture>(device_, allocator_, physical_device_, upload_,
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

    // What an unset hint_lut slot reads: the table that changes nothing.
    // Sixteen is enough, since a lookup through it is exact at any size.
    {
        auto texture = make_lut_texture(neutral_lut(16));
        const uint32_t bindless_idx = bindless_.add(*texture);
        default_lut_handle_ = textures_.insert("__default_lut", std::move(texture));
        texture_to_bindless_[default_lut_handle_.index()] = bindless_idx;
    }
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
            auto model = std::make_unique<Model>(allocator_, upload_,
                                                  std::move(data), mega_buffer_,
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
                    auto model = std::make_unique<Model>(allocator_, upload_,
                                                          std::move(data), mega_buffer_,
                                                          meshlet_buffer_);
                    return models_.insert(path, std::move(model));
                }
            }
        }

        // Fall back to glTF loading (existing path)
        auto model = std::make_unique<Model>(allocator_, device_, physical_device_, upload_,
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

    auto model = std::make_unique<Model>(allocator_, upload_, data, mega_buffer_, meshlet_buffer_);
    return models_.insert(key, std::move(model));
}

Result<TextureHandle> ResourceManager::load_texture(const std::string& path) {
    auto it = textures_.name_to_handle.find(path);
    if (it != textures_.name_to_handle.end()) {
        FJELL_GFX_DEBUG("Texture cache hit: {}", path);
        return it->second;
    }

    try {
        auto texture = std::make_unique<Texture>(device_, allocator_, physical_device_, upload_, path);
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
        auto texture = std::make_unique<Texture>(device_, allocator_, physical_device_, upload_, path, true);
        uint32_t bindless_idx = bindless_.add(*texture);
        auto handle = textures_.insert(key, std::move(texture));
        texture_to_bindless_[handle.index()] = bindless_idx;
        return handle;
    } catch (const std::exception& e) {
        return make_error("Failed to load texture (nearest) '" + path + "': " + e.what());
    }
}

uint32_t ResourceManager::fallback_texture_index(UniformHint hint) const {
    switch (hint) {
        case UniformHint::Black:     return bindless_index(default_black_handle_);
        case UniformHint::Normal:    return bindless_index(default_normal_handle_);
        case UniformHint::DefaultMr: return bindless_index(default_mr_handle_);
        case UniformHint::Lut:       return bindless_index(default_lut_handle_);
        case UniformHint::None:
        case UniformHint::Color:
        case UniformHint::White:
        case UniformHint::Enum:
            break;
    }
    return bindless_index(default_white_handle_);
}

uint32_t ResourceManager::file_texture_index(const FjslUniform& u, const std::string& file,
                                             const std::string& material_path,
                                             const Project* project) {
    if (u.hint != UniformHint::Lut) {
        auto texture = load_texture(base_path::resolve_asset(file, project));
        if (!texture) FJELL_GFX_WARN("Material '{}': {}", material_path, texture.error());
        return bindless_index(texture ? *texture : default_white_handle_);
    }
    // A grading table is not an image: it wants its 16 bits, no mips and no
    // wrap, which only the .fjlut path gives it. Any other file is refused
    // rather than read as a picture of a table.
    if (std::filesystem::path(file).extension() != ".fjlut") {
        FJELL_GFX_WARN("Material '{}': '{}' is a colour grading table and takes a .fjlut, not "
                       "'{}'; using the neutral table",
                       material_path, u.name, file);
        return fallback_texture_index(UniformHint::Lut);
    }
    auto lut = load_lut(base_path::resolve_asset(file, project));
    if (!lut) {
        FJELL_GFX_WARN("Material '{}': {}; using the neutral table", material_path, lut.error());
        return fallback_texture_index(UniformHint::Lut);
    }
    return bindless_index(*lut);
}

std::unique_ptr<Texture> ResourceManager::make_lut_texture(const LutStrip& strip) {
    // Clamped, filtered, one mip: the lookup takes its own half-texel
    // offsets and reads the table's edge texels as its ends.
    return std::make_unique<Texture>(device_, allocator_, physical_device_, upload_,
                                     reinterpret_cast<const uint8_t*>(strip.rgba.data()),
                                     strip.width(), strip.height(),
                                     VK_FORMAT_R16G16B16A16_UNORM, 1, true);
}

Result<TextureHandle> ResourceManager::load_lut(const std::string& path) {
    auto it = textures_.name_to_handle.find(path);
    if (it != textures_.name_to_handle.end()) return it->second;

    auto strip = fjell::load_lut(path);
    if (!strip) return make_error(strip.error());
    try {
        auto texture = make_lut_texture(*strip);
        const uint32_t bindless_idx = bindless_.add(*texture);
        auto handle = textures_.insert(path, std::move(texture));
        texture_to_bindless_[handle.index()] = bindless_idx;
        watch_lut(path, handle);
        return handle;
    } catch (const std::exception& e) {
        return make_error("Failed to create the table for '" + path + "': " + e.what());
    }
}

void ResourceManager::watch_lut(const std::string& path, TextureHandle handle) {
#ifndef NDEBUG
    // The recipe and its source both decide the table, and an image editor
    // saves by writing a new file over the old one, so the folders are
    // watched rather than the files.
    const std::filesystem::path recipe = path;
    std::filesystem::path source;
    if (auto parsed = read_lut_recipe(recipe)) source = lut_source_path(recipe, *parsed);
    luts_.push_back({path, source.lexically_normal().string(), handle});
    for (const auto& file : {recipe, source}) {
        if (file.empty()) continue;
        const std::string dir = std::filesystem::absolute(file).parent_path().lexically_normal().string();
        if (!lut_watched_dirs_.insert(dir).second) continue;
        lut_watcher_.watch(dir, [this](const std::filesystem::path& changed) {
            lut_changed_.push_back(std::filesystem::absolute(changed).lexically_normal().string());
        });
    }
#else
    (void)path;
    (void)handle;
#endif
}

void ResourceManager::poll_lut_changes() {
#ifndef NDEBUG
    (void)lut_watcher_.poll();
    if (lut_changed_.empty()) return;
    const auto changed = std::move(lut_changed_);
    lut_changed_.clear();
    const auto same = [](const std::string& a, const std::string& b) {
        return !a.empty() && !b.empty()
            && std::filesystem::absolute(a).lexically_normal() == std::filesystem::absolute(b).lexically_normal();
    };
    for (auto& lut : luts_) {
        const bool touched = std::any_of(changed.begin(), changed.end(), [&](const std::string& c) {
            return same(c, lut.recipe) || same(c, lut.source);
        });
        if (touched) reload_lut(lut);
    }
#endif
}

void ResourceManager::reload_lut(LoadedLut& lut) {
    // A source mid-save may be half written; the old table stays until the
    // file reads whole, and the next change event tries again.
    auto strip = fjell::load_lut(lut.recipe);
    if (!strip) {
        FJELL_GFX_WARN("LUT reload: {}", strip.error());
        return;
    }
    if (auto recipe = read_lut_recipe(lut.recipe)) {
        lut.source = lut_source_path(lut.recipe, *recipe).lexically_normal().string();
    }
    std::unique_ptr<Texture> fresh;
    try {
        fresh = make_lut_texture(*strip);
    } catch (const std::exception& e) {
        FJELL_GFX_WARN("LUT reload: {}: {}", lut.recipe, e.what());
        return;
    }
    // Same bindless index, so every material pointing at it follows; the old
    // texture lives until the frames still reading it are done, as a font
    // atlas's does on its reload.
    const uint32_t index = bindless_index(lut.handle);
    bindless_.update_entry(index, fresh->image_view(), fresh->sampler());
    auto& slot = textures_.slots[lut.handle.index()];
    if (deleter_ != nullptr) deleter_->retire(std::move(slot));
    slot = std::move(fresh);
    FJELL_GFX_INFO("LUT reloaded: {}", lut.recipe);
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
    params.has_emissive_texture = emissive.valid();
    // No shader metadata on this path, so the emission factor follows the
    // same default rule the asset loader applies: the texture shows as
    // authored when one is bound, nothing emits when none is.
    params.emission_color = derive_material_gi_params(
        ShaderMetadata{}, [](const FjslUniform&) { return std::vector<float>{}; },
        params.has_emissive_texture).emission;

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

std::vector<Material*> ResourceManager::load_slot_materials(ModelHandle model_handle,
                                                            const Project* project) {
    std::vector<Material*> slots;

    auto* model = get(model_handle);
    if (!model) return slots;

    const auto& authored = model->authored().material_slots;
    if (authored.empty()) return slots;

    slots.resize(authored.size(), nullptr);
    for (size_t i = 0; i < authored.size(); ++i) {
        if (authored[i].path.empty()) continue;

        auto resolved = base_path::resolve_asset(authored[i].path, project);
        if (resolved.empty()) {
            FJELL_GFX_WARN("Mesh material slot '{}' -> '{}': does not resolve",
                           authored[i].name, authored[i].path);
            continue;
        }

        // A slot can outlive the material it names — renamed, moved or deleted
        // since the mesh was authored. That leaves the slot unassigned and the
        // object falling back to its own material.
        auto result = load_material_asset(resolved, project);
        if (!result) {
            FJELL_GFX_WARN("Mesh material slot '{}' -> '{}': {}",
                           authored[i].name, authored[i].path, result.error());
            continue;
        }
        slots[i] = get(*result);
    }

    return slots;
}

Result<MaterialHandle> ResourceManager::load_material_asset(const std::string& path,
                                                             const Project* project) {
    auto it = materials_.name_to_handle.find(path);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    // The parser throws on a missing or malformed file. Callers get a Result,
    // so the failure belongs in it rather than escaping into whoever happens to
    // be loading a scene.
    MaterialAsset asset;
    try {
        asset = fjell::load_material_asset(path);
    } catch (const std::exception& e) {
        return make_error("Failed to load material '" + path + "': " + e.what());
    }

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
    // Compared by what actually reached the GPU rather than what the asset
    // named: a texture that failed to load is the black default too.
    params.has_emissive_texture = (em_idx != bindless_index(default_black_handle_));
    const MaterialBaseParams base = read_base_params(asset);
    params.base_color_factor = base.base_color_factor;
    params.metallic_factor = base.metallic_factor;
    params.roughness_factor = base.roughness_factor;
    params.uv_scale = base.uv_scale;
    auto material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
    material->set_pipeline_id(pipeline_id);
    material->set_shader_path(asset.shader);

    // Resolve custom params from shader metadata. The custom textures are set
    // on the final material, once its row exists, further down.
    if (shader_meta) {
        if (shader_meta->custom_param_count > 0) {
            uint32_t offset = bindless_.allocate_params(shader_meta->custom_param_count);
            material->set_custom_params(offset, shader_meta->custom_param_count);

            std::vector<float> param_data(shader_meta->custom_param_count, 0.0f);
            for (const auto& u : shader_meta->uniforms) {
                if (u.type == UniformType::Sampler2D) continue;
                uint32_t count = uniform_float_count(u.type);

                auto param_it = asset.params.find(u.name);
                if (param_it != asset.params.end()) {
                    // A value of the wrong width is a shader whose uniform
                    // changed type under a material that still holds the old
                    // shape. Copying what fits leaves the rest at zero — a
                    // vec3 given one float renders black in two channels —
                    // so say which material and what it expected.
                    if (param_it->second.size() != count) {
                        FJELL_GFX_WARN("{}: '{}' holds {} value(s) but the shader declares "
                                       "{} — the rest fall back to zero",
                                       path, u.name, param_it->second.size(), count);
                    }
                    for (uint32_t i = 0; i < count && i < param_it->second.size(); ++i) {
                        param_data[u.slot + i] = param_it->second[i];
                    }
                } else if (!u.default_value.empty()) {
                    for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                        param_data[u.slot + i] = u.default_value[i];
                    }
                }
                encode_uniform_for_gpu(u, std::span{param_data}.subspan(u.slot, count));
            }
            bindless_.write_params(offset, param_data.data(), shader_meta->custom_param_count);

            // Keys the shader no longer declares. The loop above walks the
            // shader's uniforms, so these are never read and never written
            // back — they simply sit in the file. Usually the trace of a
            // rename, where the authored value was lost the moment the
            // uniform got its new name and nothing said so.
            for (const auto& [key, values] : asset.params) {
                if (is_reserved_param(key)) continue;
                const bool declared = std::any_of(
                    shader_meta->uniforms.begin(), shader_meta->uniforms.end(),
                    [&key](const FjslUniform& u) { return u.name == key; });
                if (!declared) {
                    FJELL_GFX_WARN("{}: '{}' is not a uniform of '{}' — left over from a "
                                   "rename or a removal, and ignored",
                                   path, key, asset.shader);
                }
            }
        }
    }

    // What the ray-traced GI paths read of this material, derived from the
    // shader's tagged uniforms and this asset's values for them. A ui
    // material never meets a ray; a terrain material's rays land on its
    // layers, which are spatial materials of their own.
    if (shader_meta && shader_meta->shader_type == ShaderType::Spatial) {
        // Derived from the same linear values the params SSBO holds, so the
        // GI paths and the raster path agree on the colour.
        auto gi = derive_material_gi_params(*shader_meta, [&](const FjslUniform& u) {
            auto p_it = asset.params.find(u.name);
            std::vector<float> values =
                p_it != asset.params.end() ? p_it->second : u.default_value;
            encode_uniform_for_gpu(u, values);
            return values;
        }, params.has_emissive_texture);
        params.emission_color = gi.emission;
        params.albedo_factor = gi.albedo;
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
        material->set_shader_type(shader_meta->shader_type);
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
                    material->set_custom_texture(u.slot, file_texture_index(u, tex_path, path, project));
                }
            } else {
                material->set_custom_texture(u.slot, fallback_texture_index(u.hint));
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
        auto model = std::make_unique<SkeletalModel>(allocator_, upload_,
                                                      std::move(data), meshlet_buffer_);
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

// ── Terrain assets ───────────────────────────────────────────────────────

Result<TerrainAssetHandle> ResourceManager::load_terrain_asset(const std::string& path,
                                                               const Project* project) {
    auto it = terrain_assets_.name_to_handle.find(path);
    if (it != terrain_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto terrain = TerrainAsset::load(path, device_, allocator_, physical_device_, upload_);
    if (!terrain) {
        return make_error(terrain.error());
    }
    resolve_terrain_layers(**terrain, project);
    return terrain_assets_.insert(path, std::move(*terrain));
}

void ResourceManager::resolve_terrain_layers(TerrainAsset& terrain, const Project* project) {
    // The same plain white material a mesh gets before it is assigned one,
    // so an empty layer slot reads as untextured ground rather than as
    // whichever material happened to be created first.
    terrain.set_unassigned_layer_material(get(create_material("default", default_white_handle_)));
    for (uint32_t i = 0; i < terrain.layer_count(); ++i) {
        const std::string& path = terrain.layer_material_path(i);
        if (path.empty()) {
            terrain.set_layer_material(i, nullptr);
            continue;
        }
        auto material = load_material_asset(base_path::resolve_asset(path, project), project);
        if (!material) {
            FJELL_CORE_ERROR("terrain '{}' layer {}: {}", terrain.path(), i, material.error());
            terrain.set_layer_material(i, nullptr);
            continue;
        }
        terrain.set_layer_material(i, get(*material));
    }
    for (uint32_t i = 0; i < terrain.layer_count(); ++i) {
        const std::string& path = terrain.layer_surface_path(i);
        if (path.empty()) {
            terrain.set_layer_surface(i, 0);
            continue;
        }
        auto surface = load_surface_asset(base_path::resolve_asset(path, project));
        if (!surface) {
            FJELL_CORE_ERROR("terrain '{}' layer {} surface: {}", terrain.path(), i,
                             surface.error());
            terrain.set_layer_surface(i, 0);
            continue;
        }
        terrain.set_layer_surface(i, surface->id);
    }
}

TerrainAsset* ResourceManager::get(TerrainAssetHandle handle) const {
    return terrain_assets_.get(handle);
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

// ── Surface assets ───────────────────────────────────────────────────────

Result<SurfaceAssetHandle> ResourceManager::load_surface_asset(const std::string& path) {
    auto it = surface_assets_.name_to_handle.find(path);
    if (it != surface_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto result = load_surface_asset_file(path);
    if (!result) return make_error(result.error());
    auto asset = std::make_unique<SurfaceAsset>(std::move(*result));
    return surface_assets_.insert(path, std::move(asset));
}

SurfaceAsset* ResourceManager::get(SurfaceAssetHandle handle) const {
    return surface_assets_.get(handle);
}

std::string ResourceManager::asset_path_of(SurfaceAssetHandle handle) const {
    return surface_assets_.path_of(handle);
}

// ── Day profiles ─────────────────────────────────────────────────────────

Result<DayProfileAssetHandle> ResourceManager::load_day_profile(const std::string& path) {
    auto it = day_profiles_.name_to_handle.find(path);
    if (it != day_profiles_.name_to_handle.end()) {
        return it->second;
    }

    auto result = load_day_profile_file(base_path::resolve_asset(path, project_));
    if (!result) return make_error(result.error());
    auto asset = std::make_unique<DayProfileAsset>(std::move(*result));
    return day_profiles_.insert(path, std::move(asset));
}

Result<> ResourceManager::reload_day_profile(const std::string& path) {
    auto it = day_profiles_.name_to_handle.find(path);
    if (it == day_profiles_.name_to_handle.end()) return {};
    auto result = load_day_profile_file(base_path::resolve_asset(path, project_));
    if (!result) return make_error(result.error());
    if (auto* live = day_profiles_.get(it->second)) {
        *live = std::move(*result);
    }
    return {};
}

DayProfileAsset* ResourceManager::get(DayProfileAssetHandle handle) const {
    return day_profiles_.get(handle);
}

std::string ResourceManager::asset_path_of(DayProfileAssetHandle handle) const {
    return day_profiles_.path_of(handle);
}

// ── Weather events ───────────────────────────────────────────────────────

Result<WeatherAssetHandle> ResourceManager::load_weather_asset(const std::string& path) {
    auto it = weather_assets_.name_to_handle.find(path);
    if (it != weather_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto result = load_weather_asset_file(base_path::resolve_asset(path, project_));
    if (!result) return make_error(result.error());
    auto asset = std::make_unique<WeatherAsset>(std::move(*result));
    return weather_assets_.insert(path, std::move(asset));
}

Result<> ResourceManager::reload_weather_asset(const std::string& path) {
    auto it = weather_assets_.name_to_handle.find(path);
    if (it == weather_assets_.name_to_handle.end()) return {};
    auto result = load_weather_asset_file(base_path::resolve_asset(path, project_));
    if (!result) return make_error(result.error());
    if (auto* live = weather_assets_.get(it->second)) {
        *live = std::move(*result);
    }
    return {};
}

WeatherAsset* ResourceManager::get(WeatherAssetHandle handle) const {
    return weather_assets_.get(handle);
}

std::string ResourceManager::asset_path_of(WeatherAssetHandle handle) const {
    return weather_assets_.path_of(handle);
}

// ── Climates ─────────────────────────────────────────────────────────────

Result<ClimateAssetHandle> ResourceManager::load_climate_asset(const std::string& path) {
    auto it = climate_assets_.name_to_handle.find(path);
    if (it != climate_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto result = load_climate_asset_file(base_path::resolve_asset(path, project_));
    if (!result) return make_error(result.error());
    auto asset = std::make_unique<ClimateAsset>(std::move(*result));
    return climate_assets_.insert(path, std::move(asset));
}

Result<> ResourceManager::reload_climate_asset(const std::string& path) {
    auto it = climate_assets_.name_to_handle.find(path);
    if (it == climate_assets_.name_to_handle.end()) return {};
    auto result = load_climate_asset_file(base_path::resolve_asset(path, project_));
    if (!result) return make_error(result.error());
    if (auto* live = climate_assets_.get(it->second)) {
        *live = std::move(*result);
    }
    return {};
}

ClimateAsset* ResourceManager::get(ClimateAssetHandle handle) const {
    return climate_assets_.get(handle);
}

std::string ResourceManager::asset_path_of(ClimateAssetHandle handle) const {
    return climate_assets_.path_of(handle);
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

Result<PresetAssetHandle> ResourceManager::load_preset_asset(const std::string& path,
                                                             const Project* project) {
    auto it = preset_assets_.name_to_handle.find(path);
    if (it != preset_assets_.name_to_handle.end()) {
        return it->second;
    }

    auto result = Preset::load(path, *this, project ? project : project_);
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
