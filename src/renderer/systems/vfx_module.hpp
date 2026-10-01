#pragma once

#include "ftk/math/curve.hpp"
#include "renderer/systems/vfx_pin.hpp"
#include "renderer/resources/fjsl_parser.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fjell {

// Module types — must match the GLSL MOD_* constants in particle_modules.glsl
enum class ModuleType : uint32_t {
    // Spawn modules
    spawn_rate         = 0,
    initial_lifetime   = 1,
    initial_position   = 2,
    initial_velocity   = 3,
    initial_color      = 4,
    initial_size       = 5,
    initial_stretch    = 6,
    burst              = 7,
    initial_rotation   = 8,
    initial_texture    = 9,
    flipbook           = 10,
    velocity_inherit   = 11,
    emit_over_distance = 12,

    // Update modules
    gravity            = 100,
    color_over_life    = 101,
    size_over_life     = 102,
    drag               = 103,
    noise              = 104,
    attract            = 105,
    velocity_limit     = 106,
    collision          = 107,
    curve_color_over_life = 108,
    curve_size_over_life  = 109,
    wind                  = 110,
    orbital               = 111,
    emissive              = 112,
    sub_emitter           = 113,

    // Render modules
    billboard          = 200,
    trail              = 201,
    lit_particle       = 202,
    mesh_particle      = 203,
    distortion         = 204,
};

// Position shape for InitialPosition module
enum class SpawnShape : uint32_t {
    point  = 0,
    sphere = 1,
    box    = 2,
    cone   = 3,
    ring   = 4,
    edge   = 5,
    disk   = 6,
};

// Velocity mode for InitialVelocity module
enum class VelocityMode : uint32_t {
    directional = 0,
    random      = 1,
    cone        = 2,
};

/// Base class for VFX modules. Each module represents a small behavior unit
/// in an emitter's spawn/update/render stack.
struct VfxModule {
    virtual ~VfxModule() = default;

    [[nodiscard]] virtual ModuleType type() const = 0;
    [[nodiscard]] virtual const char* name() const = 0;

    /// Write module parameters to a GPU buffer (header + params).
    /// Returns total bytes written (must be a multiple of 4).
    virtual uint32_t write_gpu_data(uint8_t* buffer) const = 0;

    /// An asset reference a module holds, surfaced for the editor to draw
    /// (its fields are drawn in ui/inspectors/vfx_module_inspectors.cpp).
    /// Points at the module's own member, so the widget writes through it.
    struct AssetField {
        const char* label;
        std::string* reference;
        std::span<const std::string> extensions;
        /// For a material or shader: the Types the module can draw it as.
        std::span<const ShaderType> shader_types{};
    };

    /// Asset references this module holds. Empty for modules with none.
    [[nodiscard]] virtual std::vector<AssetField> asset_fields() { return {}; }

    // Extension sets for asset_fields(), so the accepted types are stated once.
    [[nodiscard]] static std::span<const std::string> texture_extensions();
    [[nodiscard]] static std::span<const std::string> material_extensions();
    [[nodiscard]] static std::span<const std::string> mesh_extensions();

    /// Describe parameters as typed pins for the node graph.
    [[nodiscard]] virtual std::vector<PinDescriptor> pin_descriptors() { return {}; }

    /// JSON serialization
    virtual void to_json(nlohmann::json& j) const = 0;
    virtual void from_json(const nlohmann::json& j) = 0;

    bool enabled{true};

    // Editor node position (serialized so layout persists)
    float editor_x{0.0f};
    float editor_y{0.0f};
    bool position_set{false};
};

/// Create a module instance by type name (for JSON deserialization)
[[nodiscard]] std::unique_ptr<VfxModule> create_module(const std::string& type_name);

/// GPU module header (8 bytes, matches GLSL ModuleHeader)
struct GpuModuleHeader {
    uint32_t type;
    uint32_t size; // total size in uint32_t units (including header)
};

// ─── Concrete modules ────────────────────────────────────────────────────

struct SpawnRateModule final : VfxModule {
    float rate{50.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::spawn_rate; }
    [[nodiscard]] const char* name() const override { return "Spawn Rate"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialLifetimeModule final : VfxModule {
    float lifetime_min{1.0f};
    float lifetime_max{3.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_lifetime; }
    [[nodiscard]] const char* name() const override { return "Initial Lifetime"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialPositionModule final : VfxModule {
    SpawnShape shape{SpawnShape::point};
    float radius{0.1f};           // sphere, cone, ring, disk
    glm::vec3 half_extents{0.5f}; // box
    float edge_length{1.0f};      // edge (centered, along local X)

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_position; }
    [[nodiscard]] const char* name() const override { return "Initial Position"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialVelocityModule final : VfxModule {
    VelocityMode mode{VelocityMode::random};
    glm::vec3 velocity_min{-0.5f, 1.0f, -0.5f};
    glm::vec3 velocity_max{0.5f, 3.0f, 0.5f};
    // Cone mode params
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    float angle{30.0f};        // half-angle in degrees
    float speed_min{2.0f};
    float speed_max{5.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_velocity; }
    [[nodiscard]] const char* name() const override { return "Initial Velocity"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialColorModule final : VfxModule {
    glm::vec4 color{1.0f, 0.8f, 0.2f, 1.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_color; }
    [[nodiscard]] const char* name() const override { return "Initial Color"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialSizeModule final : VfxModule {
    float size_min{0.05f};
    float size_max{0.1f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_size; }
    [[nodiscard]] const char* name() const override { return "Initial Size"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct GravityModule final : VfxModule {
    glm::vec3 force{0.0f, -9.81f, 0.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::gravity; }
    [[nodiscard]] const char* name() const override { return "Gravity"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct WindModule final : VfxModule {
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    float strength{1.0f};
    // Follow the environment's wind: direction from the weather, strength
    // scaled by its speed and gusts, so every effect leans the same way.
    bool use_global{false};

    [[nodiscard]] ModuleType type() const override { return ModuleType::wind; }
    [[nodiscard]] const char* name() const override { return "Wind"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

enum class OrbitalMode : uint32_t {
    vortex = 0, // spin around axis
    orbit  = 1, // orbit center point
    swirl  = 2, // vortex + inward pull
};

struct OrbitalModule final : VfxModule {
    OrbitalMode mode{OrbitalMode::vortex};
    glm::vec3 axis{0.0f, 1.0f, 0.0f};   // rotation axis (vortex/swirl) or center (orbit)
    float speed{2.0f};                    // angular speed (radians/sec)
    float pull{0.0f};                     // inward pull strength (swirl mode)

    [[nodiscard]] ModuleType type() const override { return ModuleType::orbital; }
    [[nodiscard]] const char* name() const override { return "Orbital"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct EmissiveModule final : VfxModule {
    glm::vec3 color{1.0f, 0.8f, 0.3f};
    float intensity{5.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::emissive; }
    [[nodiscard]] const char* name() const override { return "Emissive"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

enum class SubEmitterTrigger : uint32_t {
    death     = 0,
    collision = 1,
    both      = 2,
};

struct SubEmitterModule final : VfxModule {
    SubEmitterTrigger trigger{SubEmitterTrigger::death};
    uint32_t child_emitter_index{1}; // index into VfxEffect::emitters
    uint32_t burst_count{5};
    float velocity_inherit{0.5f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::sub_emitter; }
    [[nodiscard]] const char* name() const override { return "Sub-Emitter"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct ColorOverLifeModule final : VfxModule {
    glm::vec4 color_start{1.0f, 0.8f, 0.2f, 1.0f};
    glm::vec4 color_end{1.0f, 0.1f, 0.0f, 0.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::color_over_life; }
    [[nodiscard]] const char* name() const override { return "Color Over Life"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct SizeOverLifeModule final : VfxModule {
    float size_start{0.1f};
    float size_end{0.02f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::size_over_life; }
    [[nodiscard]] const char* name() const override { return "Size Over Life"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct DragModule final : VfxModule {
    float drag{0.1f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::drag; }
    [[nodiscard]] const char* name() const override { return "Drag"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct NoiseModule final : VfxModule {
    float strength{1.0f};
    float frequency{2.0f};
    float scroll_speed{0.5f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::noise; }
    [[nodiscard]] const char* name() const override { return "Noise"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct AttractModule final : VfxModule {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    float strength{5.0f};
    float radius{10.0f};   // falloff radius

    [[nodiscard]] ModuleType type() const override { return ModuleType::attract; }
    [[nodiscard]] const char* name() const override { return "Attract"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct VelocityLimitModule final : VfxModule {
    float max_speed{10.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::velocity_limit; }
    [[nodiscard]] const char* name() const override { return "Velocity Limit"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct CollisionModule final : VfxModule {
    enum class Response : uint32_t {
        bounce = 0,
        slide  = 1,
        kill   = 2,
    };

    enum class Source : uint32_t {
        sdf   = 0,  // 3D SDF volume (rest-pose geometry, works everywhere)
        depth = 1,  // depth buffer (handles displacement, screen-space only)
        both  = 2,  // SDF + depth fallback
        // What stands between the particle and the sky. Nothing measures
        // shelter at present, so a particle set to this collides with
        // nothing; the value is kept so effects that name it still load.
        overhead = 3,
    };

    Response response{Response::bounce};
    Source source{Source::sdf};
    float restitution{0.5f};     // bounciness (0-1)
    float friction{0.2f};        // tangential damping (0-1)
    float radius_scale{0.5f};    // collision radius as a fraction of particle size

    // Set by particle system each frame from SceneSdfPass bounds
    glm::vec3 sdf_volume_min{-10.0f};
    glm::vec3 sdf_volume_max{10.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::collision; }
    [[nodiscard]] const char* name() const override { return "Collision"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialStretchModule final : VfxModule {
    float stretch{0.0f}; // 0 = billboard, >0 = velocity-aligned streak

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_stretch; }
    [[nodiscard]] const char* name() const override { return "Initial Stretch"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialTextureModule final : VfxModule {
    std::string texture_path;         // relative asset path (serialized)
    uint32_t bindless_index{0};       // resolved at runtime

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_texture; }
    [[nodiscard]] const char* name() const override { return "Initial Texture"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    [[nodiscard]] std::vector<AssetField> asset_fields() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialRotationModule final : VfxModule {
    float rotation_min{0.0f};       // radians
    float rotation_max{6.2832f};    // full turn
    float speed{0.0f};              // radians/sec
    float speed_variance{0.0f};     // +/- range

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_rotation; }
    [[nodiscard]] const char* name() const override { return "Initial Rotation"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct BurstModule final : VfxModule {
    uint32_t count{100};

    [[nodiscard]] ModuleType type() const override { return ModuleType::burst; }
    [[nodiscard]] const char* name() const override { return "Burst"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct EmitOverDistanceModule final : VfxModule {
    float particles_per_meter{10.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::emit_over_distance; }
    [[nodiscard]] const char* name() const override { return "Emit Over Distance"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct FlipbookModule final : VfxModule {
    uint32_t columns{4};
    uint32_t rows{4};
    uint32_t frame_count{0};   // 0 = columns * rows
    float fps{24.0f};

    enum class Mode : uint32_t {
        over_lifetime = 0,  // frame = age * total_frames
        fps_loop      = 1,  // fixed rate, wraps
        fps_clamp     = 2,  // fixed rate, stops on last frame
    };
    Mode mode{Mode::over_lifetime};

    [[nodiscard]] uint32_t pack_params() const;

    [[nodiscard]] ModuleType type() const override { return ModuleType::flipbook; }
    [[nodiscard]] const char* name() const override { return "Flipbook"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct BillboardModule final : VfxModule {
    [[nodiscard]] ModuleType type() const override { return ModuleType::billboard; }
    [[nodiscard]] const char* name() const override { return "Billboard"; }
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct VelocityInheritanceModule final : VfxModule {
    float factor{1.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::velocity_inherit; }
    [[nodiscard]] const char* name() const override { return "Velocity Inheritance"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

/// 32-sample LUT per channel. GPU reads with linear interpolation.
constexpr uint32_t kCurveLutSamples = 32;

struct CurveColorOverLifeModule final : VfxModule {
    Curve curve_r{Curve::linear(1.0f, 1.0f)};
    Curve curve_g{Curve::linear(0.8f, 0.1f)};
    Curve curve_b{Curve::linear(0.2f, 0.0f)};
    Curve curve_a{Curve::linear(1.0f, 0.0f)};

    [[nodiscard]] ModuleType type() const override { return ModuleType::curve_color_over_life; }
    [[nodiscard]] const char* name() const override { return "Curve Color Over Life"; }
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

enum class ParticleMeshShape : uint32_t {
    cube   = 0,
    sphere = 1,
    custom = 2,
};

struct MeshParticleModule final : VfxModule {
    ParticleMeshShape shape{ParticleMeshShape::cube};
    std::string model_path;     // only used when shape == custom
    std::string material_path;  // .fjmat material for PBR textures
    uint32_t resolved_material_id{0}; // runtime: resolved from material_path

    [[nodiscard]] ModuleType type() const override { return ModuleType::mesh_particle; }
    [[nodiscard]] const char* name() const override { return "Mesh Particle"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    [[nodiscard]] std::vector<AssetField> asset_fields() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct LitParticleModule final : VfxModule {
    float metallic{0.0f};
    float roughness{0.5f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::lit_particle; }
    [[nodiscard]] const char* name() const override { return "Lit Particle"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct DistortionModule final : VfxModule {
    float strength{0.05f};
    std::string normal_map_path;
    uint32_t normal_map_index{0}; // resolved bindless index

    [[nodiscard]] ModuleType type() const override { return ModuleType::distortion; }
    [[nodiscard]] const char* name() const override { return "Distortion"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    [[nodiscard]] std::vector<AssetField> asset_fields() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct TrailModule final : VfxModule {
    float width{0.1f};
    float width_over_life{1.0f};  // 1.0 = constant, 0.0 = taper to zero at death
    bool face_camera{true};

    [[nodiscard]] ModuleType type() const override { return ModuleType::trail; }
    [[nodiscard]] const char* name() const override { return "Trail"; }
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct CurveSizeOverLifeModule final : VfxModule {
    Curve curve{Curve::linear(0.1f, 0.02f)};

    [[nodiscard]] ModuleType type() const override { return ModuleType::curve_size_over_life; }
    [[nodiscard]] const char* name() const override { return "Curve Size Over Life"; }
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

} // namespace fjell
