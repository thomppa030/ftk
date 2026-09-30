#pragma once

#include "core/input_action.hpp"
#include "core/result.hpp"
#include "platform/file_watcher.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/resources/resource_pool.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/uniform_types.hpp"
#include "audio/audio_asset.hpp"
#include "physics/surface_asset.hpp"
#include "environment/day_profile_asset.hpp"
#include "environment/climate_asset.hpp"
#include "environment/weather_asset.hpp"
// Complete type required: ResourcePool<TerrainAsset> destroys unique_ptrs inline.
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "scene/preset_asset.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class BindlessTextures;
namespace gpu {
class Device;
}
struct LutStrip;
class MegaBuffer;
class MeshletBuffer;
class Model;
class PipelineCache;
class Project;
class Texture;
class Material;
class TerrainAsset;
struct MaterialAsset;

class ResourceManager {
public:
    ResourceManager(gpu::Device& gpu,
                    BindlessTextures& bindless, MegaBuffer* mega_buffer = nullptr,
                    MeshletBuffer* meshlet_buffer = nullptr,
                    PipelineCache* pipeline_cache = nullptr);

    void set_cache_dir(const std::filesystem::path& dir) { cache_dir_ = dir; }

    Result<ModelHandle> load_model(const std::string& path);
    Result<ModelHandle> create_primitive(const std::string& type);
    Result<TextureHandle> load_texture(const std::string& path);
    Result<TextureHandle> load_texture_nearest(const std::string& path);
    /// A colour grading table from a `.fjlut` recipe, as a 16-bit strip:
    /// clamped, filtered, one mip. Cached by path. In debug builds an edit to
    /// the recipe or its source reloads it in place through
    /// poll_lut_changes(), keeping its bindless index.
    Result<TextureHandle> load_lut(const std::string& path);
    /// Re-read every loaded table whose recipe or source changed on disk.
    /// Debug builds; once a frame, before anything records.
    void poll_lut_changes();

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
    /// Resolves a model's authored material slots to loaded materials.
    /// Slots with no assignment, or whose .fjmat fails to load, come back
    /// null so the caller can fall back to the object's own material.
    [[nodiscard]] std::vector<Material*> load_slot_materials(ModelHandle model,
                                                            const Project* project);

    Result<MaterialHandle> load_material_asset(const std::string& path,
                                               const Project* project = nullptr);

    // VFX assets (.fjvfx)
    Result<VfxAssetHandle> load_vfx_asset(const std::string& path);

    // Terrain assets (.fjterrain). project resolves the layer materials the
    // file names relative to the project root.
    Result<TerrainAssetHandle> load_terrain_asset(const std::string& path,
                                                  const Project* project = nullptr);

    /// Load (or reload) the .fjmat and .fjsurface behind each of a terrain's
    /// layers and bind them to the asset. Called after load and whenever the
    /// inspector changes a layer's path. A layer whose material fails to load
    /// is left unbound and draws with the fallback material; a surface that
    /// fails to load leaves the layer with none.
    void resolve_terrain_layers(TerrainAsset& terrain, const Project* project);

    // Audio assets (.fjaudio)
    Result<AudioAssetHandle> load_audio_asset(const std::string& path);

    // Surface assets (.fjsurface). The handle's id is the surface id physics
    // reports, so loading through here is what gives a surface its identity.
    Result<SurfaceAssetHandle> load_surface_asset(const std::string& path);

    // Day profiles (.fjday). A reload replaces the pooled asset in place, so
    // a World Environment node keeps pointing at the same object.
    Result<DayProfileAssetHandle> load_day_profile(const std::string& path);
    Result<> reload_day_profile(const std::string& path);

    // Weather events (.fjweather). Reloads replace the pooled asset in place.
    Result<WeatherAssetHandle> load_weather_asset(const std::string& path);
    Result<> reload_weather_asset(const std::string& path);

    // Climates (.fjclimate). Reloads replace the pooled asset in place.
    Result<ClimateAssetHandle> load_climate_asset(const std::string& path);
    Result<> reload_climate_asset(const std::string& path);

    // Input action assets (.fjaction)
    Result<InputActionHandle> load_input_action(const std::string& path);

    // Preset assets (.fjp). project resolves asset paths inside the preset
    // (skeletal meshes, materials, …) relative to the project root.
    Result<PresetAssetHandle> load_preset_asset(const std::string& path,
                                                const Project* project = nullptr);


    // Generic asset loading by ref_type_name (for inspector picker)
    // Returns raw handle id (0 on failure). Caller must cast to the right Handle type.
    Result<uint32_t> load_asset_by_type(const std::string& type_name, const std::string& path);

    // Reverse lookup: handle → path
    [[nodiscard]] std::string asset_path_of(VfxAssetHandle handle) const;
    [[nodiscard]] std::string asset_path_of(AudioAssetHandle handle) const;
    [[nodiscard]] std::string asset_path_of(SurfaceAssetHandle handle) const;
    [[nodiscard]] std::string asset_path_of(DayProfileAssetHandle handle) const;
    [[nodiscard]] std::string asset_path_of(WeatherAssetHandle handle) const;
    [[nodiscard]] std::string asset_path_of(ClimateAssetHandle handle) const;
    [[nodiscard]] std::string asset_path_of(MaterialHandle handle) const;
    [[nodiscard]] std::string asset_path_of(InputActionHandle handle) const;
    [[nodiscard]] std::string asset_path_of(PresetAssetHandle handle) const;

    /// The handle kept for `path` among the assets of `HandleT`'s kind,
    /// whether it loaded or failed to: a failed load keeps its slot, so a
    /// field holding it still names the file. Invalid when nothing was ever
    /// loaded under `path`.
    template <typename HandleT>
    [[nodiscard]] HandleT asset_handle_of(const std::string& path) const {
        if constexpr (std::is_same_v<HandleT, VfxAssetHandle>) return vfx_assets_.find(path);
        else if constexpr (std::is_same_v<HandleT, AudioAssetHandle>) return audio_assets_.find(path);
        else if constexpr (std::is_same_v<HandleT, SurfaceAssetHandle>) return surface_assets_.find(path);
        else if constexpr (std::is_same_v<HandleT, DayProfileAssetHandle>) return day_profiles_.find(path);
        else if constexpr (std::is_same_v<HandleT, WeatherAssetHandle>) return weather_assets_.find(path);
        else if constexpr (std::is_same_v<HandleT, ClimateAssetHandle>) return climate_assets_.find(path);
        else if constexpr (std::is_same_v<HandleT, MaterialHandle>) return materials_.find(path);
        else if constexpr (std::is_same_v<HandleT, InputActionHandle>) return input_actions_.find(path);
        else if constexpr (std::is_same_v<HandleT, PresetAssetHandle>) return preset_assets_.find(path);
        else static_assert(sizeof(HandleT) == 0, "not a kind of asset scripts refer to");
    }

    [[nodiscard]] Model* get(ModelHandle handle) const;
    [[nodiscard]] SkeletalModel* get(SkeletalModelHandle handle) const;
    [[nodiscard]] Texture* get(TextureHandle handle) const;
    [[nodiscard]] Material* get(MaterialHandle handle) const;
    [[nodiscard]] VfxEffect* get(VfxAssetHandle handle) const;
    [[nodiscard]] TerrainAsset* get(TerrainAssetHandle handle) const;
    [[nodiscard]] AudioAsset* get(AudioAssetHandle handle) const;
    [[nodiscard]] SurfaceAsset* get(SurfaceAssetHandle handle) const;
    [[nodiscard]] DayProfileAsset* get(DayProfileAssetHandle handle) const;
    [[nodiscard]] WeatherAsset* get(WeatherAssetHandle handle) const;
    [[nodiscard]] ClimateAsset* get(ClimateAssetHandle handle) const;
    [[nodiscard]] InputActionDef* get(InputActionHandle handle) const;
    [[nodiscard]] struct PresetAsset* get(PresetAssetHandle handle) const;

    [[nodiscard]] MaterialHandle find_material(const std::string& name) const;

    [[nodiscard]] TextureHandle default_white_handle() const { return default_white_handle_; }
    [[nodiscard]] TextureHandle default_normal_handle() const { return default_normal_handle_; }
    [[nodiscard]] TextureHandle default_metallic_roughness_handle() const { return default_mr_handle_; }
    [[nodiscard]] TextureHandle default_black_handle() const { return default_black_handle_; }
    /// The table that changes nothing, what an unset hint_lut slot reads.
    [[nodiscard]] TextureHandle default_lut_handle() const { return default_lut_handle_; }
    /// The bindless index a sampler with this hint reads when nothing is
    /// bound to it: white, black, a flat normal, the default
    /// metallic/roughness, or the neutral table.
    [[nodiscard]] uint32_t fallback_texture_index(UniformHint hint) const;

    [[nodiscard]] uint32_t bindless_index(TextureHandle handle) const;

    // Lower-level accessors used by material-instance and script-API code
    // that needs to allocate new SSBO rows / look up shader metadata.
    [[nodiscard]] BindlessTextures& bindless() { return bindless_; }
    [[nodiscard]] PipelineCache* pipeline_cache() { return pipeline_cache_; }

    /// Set a callback that resolves "$viewport:name" texture references to bindless indices.
    using ViewportResolver = std::function<uint32_t(const std::string&)>;
    void set_viewport_resolver(ViewportResolver resolver);

    // Active project, used as the default for resolving relative asset paths
    // inside loaded assets (presets, materials) when no project is passed
    // explicitly. Set when the project opens.
    void set_project(const Project* project) { project_ = project; }

    /// Re-resolve all materials that reference "$viewport:name" with the given bindless index.
    /// Call after a scene viewport is created so materials pick up the new texture.
    void update_viewport_bindings(const std::string& viewport_name, uint32_t bindless_idx);

private:
    /// Uploads what is loaded, and keeps a texture being replaced alive
    /// until the frames still reading it are done.
    gpu::Device& gpu_;
    BindlessTextures& bindless_;
    MegaBuffer* mega_buffer_;
    MeshletBuffer* meshlet_buffer_;
    PipelineCache* pipeline_cache_;
    std::filesystem::path cache_dir_;

    ResourcePool<Model, ModelHandle> models_;
    ResourcePool<SkeletalModel, SkeletalModelHandle> skeletal_models_;
    ResourcePool<Texture, TextureHandle> textures_;
    ResourcePool<Material, MaterialHandle> materials_;
    ResourcePool<VfxEffect, VfxAssetHandle> vfx_assets_;
    ResourcePool<TerrainAsset, TerrainAssetHandle> terrain_assets_;
    ResourcePool<AudioAsset, AudioAssetHandle> audio_assets_;
    ResourcePool<SurfaceAsset, SurfaceAssetHandle> surface_assets_;
    ResourcePool<DayProfileAsset, DayProfileAssetHandle> day_profiles_;
    ResourcePool<WeatherAsset, WeatherAssetHandle> weather_assets_;
    ResourcePool<ClimateAsset, ClimateAssetHandle> climate_assets_;
    ResourcePool<InputActionDef, InputActionHandle> input_actions_;
    ResourcePool<PresetAsset, PresetAssetHandle> preset_assets_;

    // Maps texture pool slot index → bindless texture index
    std::unordered_map<uint32_t, uint32_t> texture_to_bindless_;

    TextureHandle default_white_handle_;
    TextureHandle default_normal_handle_;
    TextureHandle default_mr_handle_;
    TextureHandle default_black_handle_;
    TextureHandle default_lut_handle_;

    ViewportResolver viewport_resolver_;
    const Project*   project_{nullptr};  // default for relative asset-path resolution

    // Tracks which materials reference which viewport, for deferred binding.
    // Key: viewport name, Value: list of {material_id, custom_texture_slot}
    struct ViewportBinding { uint32_t material_id; uint32_t slot; };
    std::unordered_map<std::string, std::vector<ViewportBinding>> viewport_bindings_;

    /// The bindless index for a file a material names for one of its
    /// shader's samplers, loaded as the uniform's hint asks: a `.fjlut` for a
    /// hint_lut slot, an image for any other.
    uint32_t file_texture_index(const FjslUniform& u, const std::string& file,
                                const std::string& material_path, const Project* project);
    std::unique_ptr<Texture> make_lut_texture(const LutStrip& strip);

    /// A loaded table and the files it is made from, for the reload.
    struct LoadedLut {
        std::string recipe;
        std::string source;
        TextureHandle handle;
    };
    void watch_lut(const std::string& path, TextureHandle handle);
    void reload_lut(LoadedLut& lut);

    std::vector<LoadedLut> luts_;
    platform::FileWatcher lut_watcher_;
    std::unordered_set<std::string> lut_watched_dirs_;
    std::vector<std::string> lut_changed_;
};

} // namespace fjell
