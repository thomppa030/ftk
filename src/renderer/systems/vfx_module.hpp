#pragma once

#include "renderer/systems/vfx_pin.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <memory>
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

    // Update modules
    gravity            = 100,
    color_over_life    = 101,
    size_over_life     = 102,
    drag               = 103,
    noise              = 104,
    attract            = 105,
    velocity_limit     = 106,
    collision          = 107,

    // Render modules
    billboard          = 200,
};

// Position shape for InitialPosition module
enum class SpawnShape : uint32_t {
    point  = 0,
    sphere = 1,
    box    = 2,
    cone   = 3,
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

    /// Draw ImGui inspector. Returns true if any parameter changed.
    virtual bool draw_inspector() = 0;

    /// Describe parameters as typed pins for the node graph.
    [[nodiscard]] virtual std::vector<PinDescriptor> pin_descriptors() { return {}; }

    /// JSON serialization
    virtual void to_json(nlohmann::json& j) const = 0;
    virtual void from_json(const nlohmann::json& j) = 0;

    bool enabled{true};
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
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialPositionModule final : VfxModule {
    SpawnShape shape{SpawnShape::point};
    float radius{0.1f};        // sphere
    glm::vec3 half_extents{0.5f}; // box

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_position; }
    [[nodiscard]] const char* name() const override { return "Initial Position"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialColorModule final : VfxModule {
    glm::vec4 color{1.0f, 0.8f, 0.2f, 1.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_color; }
    [[nodiscard]] const char* name() const override { return "Initial Color"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct GravityModule final : VfxModule {
    glm::vec3 force{0.0f, -9.81f, 0.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::gravity; }
    [[nodiscard]] const char* name() const override { return "Gravity"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct DragModule final : VfxModule {
    float drag{0.1f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::drag; }
    [[nodiscard]] const char* name() const override { return "Drag"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct VelocityLimitModule final : VfxModule {
    float max_speed{10.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::velocity_limit; }
    [[nodiscard]] const char* name() const override { return "Velocity Limit"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    };

    Response response{Response::bounce};
    Source source{Source::sdf};
    float restitution{0.5f};     // bounciness (0-1)
    float friction{0.2f};        // tangential damping (0-1)

    [[nodiscard]] ModuleType type() const override { return ModuleType::collision; }
    [[nodiscard]] const char* name() const override { return "Collision"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialStretchModule final : VfxModule {
    float stretch{0.0f}; // 0 = billboard, >0 = velocity-aligned streak

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_stretch; }
    [[nodiscard]] const char* name() const override { return "Initial Stretch"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct BurstModule final : VfxModule {
    uint32_t count{100};

    [[nodiscard]] ModuleType type() const override { return ModuleType::burst; }
    [[nodiscard]] const char* name() const override { return "Burst"; }
    [[nodiscard]] std::vector<PinDescriptor> pin_descriptors() override;
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
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
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct BillboardModule final : VfxModule {
    [[nodiscard]] ModuleType type() const override { return ModuleType::billboard; }
    [[nodiscard]] const char* name() const override { return "Billboard"; }
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

} // namespace fjell
