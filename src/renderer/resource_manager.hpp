#pragma once

#include "core/result.hpp"
#include "renderer/resource_handle.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/uniform_types.hpp"
#include "audio/audio_asset.hpp"
#include "renderer/vfx_asset.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class BindlessTextures;
class MegaBuffer;
class MeshletBuffer;
class Model;
class PipelineCache;
class Project;
class Texture;
class Material;
struct MaterialAsset;

class ResourceManager {
public:
    ResourceManager(VkDevice device, VkPhysicalDevice physical_device,
                    VkCommandPool command_pool, VkQueue queue,
                    BindlessTextures& bindless, MegaBuffer* mega_buffer = nullptr,
                    MeshletBuffer* meshlet_buffer = nullptr,
                    PipelineCache* pipeline_cache = nullptr);

    void set_cache_dir(const std::filesystem::path& dir) { cache_dir_ = dir; }

    Result<ModelHandle> load_model(const std::string& path);
    Result<ModelHandle> create_primitive(const std::string& type);
    Result<TextureHandle> load_texture(const std::string& path);

    // Async loading support
    [[nodiscard]] ModelHandle find_model(const std::string& path) const;
    ModelHandle reserve_model(const std::string& path);
    void emplace_model(ModelHandle handle, std::unique_ptr<Model> model);
    void fail_model(ModelHandle handle);
    [[nodiscard]] LoadState model_state(ModelHandle handle) const;

    // Skeletal models
    Result<SkeletalModelHandle> load_skeletal_model(const std::string& path);
    [[nodiscard]] SkeletalModelHandle find_skeletal_model(const std::string& path) const;
    SkeletalModelHandle reserve_skeletal_model(const std::string& path);
    void emplace_skeletal_model(SkeletalModelHandle handle, std::unique_ptr<SkeletalModel> model);
    void fail_skeletal_model(SkeletalModelHandle handle);
    [[nodiscard]] LoadState skeletal_model_state(SkeletalModelHandle handle) const;

    MaterialHandle create_material(const std::string& name,
                                   TextureHandle base_color,
                                   TextureHandle normal_map = {},
                                   TextureHandle metallic_roughness = {},
                                   TextureHandle emissive = {},
                                   PbrParams params = {});

    // Create material using a raw texture pointer (for model-embedded textures)
    MaterialHandle create_material_from_texture(const std::string& name,
                                                Texture* base_color,
                                                PbrParams params = {});

    // Load a .fjmat material asset, resolving shader + textures + params.
    // If project is provided, asset paths inside .fjmat are resolved relative to it.
    Result<MaterialHandle> load_material_asset(const std::string& path,
                                               const Project* project = nullptr);

    // VFX assets (.fjvfx)
    Result<VfxAssetHandle> load_vfx_asset(const std::string& path);

    // Audio assets (.fjaudio)
    Result<AudioAssetHandle> load_audio_asset(const std::string& path);

    [[nodiscard]] Model* get(ModelHandle handle) const;
    [[nodiscard]] SkeletalModel* get(SkeletalModelHandle handle) const;
    [[nodiscard]] Texture* get(TextureHandle handle) const;
    [[nodiscard]] Material* get(MaterialHandle handle) const;
    [[nodiscard]] VfxAsset* get(VfxAssetHandle handle) const;
    [[nodiscard]] AudioAsset* get(AudioAssetHandle handle) const;

    [[nodiscard]] MaterialHandle find_material(const std::string& name) const;

    [[nodiscard]] TextureHandle default_white_handle() const { return default_white_handle_; }
    [[nodiscard]] TextureHandle default_normal_handle() const { return default_normal_handle_; }
    [[nodiscard]] TextureHandle default_metallic_roughness_handle() const { return default_mr_handle_; }
    [[nodiscard]] TextureHandle default_black_handle() const { return default_black_handle_; }

    [[nodiscard]] uint32_t bindless_index(TextureHandle handle) const;

private:
    template <typename T, typename HandleT>
    struct Pool {
        std::vector<std::unique_ptr<T>> slots;
        std::vector<uint32_t> generations;
        std::vector<LoadState> states;
        std::unordered_map<std::string, HandleT> name_to_handle;

        HandleT insert(const std::string& key, std::unique_ptr<T> resource);
        T* get(HandleT handle) const;

        // Reserve a slot (nullptr + Loading state), returns handle immediately
        HandleT reserve(const std::string& key);

        // Fill a previously reserved slot
        void emplace(HandleT handle, std::unique_ptr<T> resource);

        // Query load state
        [[nodiscard]] LoadState state(HandleT handle) const;
    };

    VkDevice device_;
    VkPhysicalDevice physical_device_;
    VkCommandPool command_pool_;
    VkQueue queue_;
    BindlessTextures& bindless_;
    MegaBuffer* mega_buffer_;
    MeshletBuffer* meshlet_buffer_;
    PipelineCache* pipeline_cache_;
    std::filesystem::path cache_dir_;

    Pool<Model, ModelHandle> models_;
    Pool<SkeletalModel, SkeletalModelHandle> skeletal_models_;
    Pool<Texture, TextureHandle> textures_;
    Pool<Material, MaterialHandle> materials_;
    Pool<VfxAsset, VfxAssetHandle> vfx_assets_;
    Pool<AudioAsset, AudioAssetHandle> audio_assets_;

    // Maps texture pool slot index → bindless texture index
    std::unordered_map<uint32_t, uint32_t> texture_to_bindless_;

    TextureHandle default_white_handle_;
    TextureHandle default_normal_handle_;
    TextureHandle default_mr_handle_;
    TextureHandle default_black_handle_;
};

} // namespace fjell
