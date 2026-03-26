#pragma once

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

    // Update modules
    gravity            = 100,
    color_over_life    = 101,
    size_over_life     = 102,
    drag               = 103,
    noise              = 104,
    attract            = 105,
    velocity_limit     = 106,

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
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct InitialColorModule final : VfxModule {
    glm::vec4 color{1.0f, 0.8f, 0.2f, 1.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::initial_color; }
    [[nodiscard]] const char* name() const override { return "Initial Color"; }
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
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct GravityModule final : VfxModule {
    glm::vec3 force{0.0f, -9.81f, 0.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::gravity; }
    [[nodiscard]] const char* name() const override { return "Gravity"; }
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
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct DragModule final : VfxModule {
    float drag{0.1f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::drag; }
    [[nodiscard]] const char* name() const override { return "Drag"; }
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
    uint32_t write_gpu_data(uint8_t* buffer) const override;
    bool draw_inspector() override;
    void to_json(nlohmann::json& j) const override;
    void from_json(const nlohmann::json& j) override;
};

struct VelocityLimitModule final : VfxModule {
    float max_speed{10.0f};

    [[nodiscard]] ModuleType type() const override { return ModuleType::velocity_limit; }
    [[nodiscard]] const char* name() const override { return "Velocity Limit"; }
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
