#pragma once

#include "core/command.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class CommandHistory;
class DebugDrawQueue;
class PhysicsSubsystem;
class TerrainAsset;
class UploadContext;

/// A rectangle of samples on one of the terrain's grids — height samples or
/// weight texels — in that grid's units.
struct HeightRect {
    uint32_t x{0};
    uint32_t z{0};
    uint32_t width{0};
    uint32_t height{0};

    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
    [[nodiscard]] size_t count() const { return static_cast<size_t>(width) * height; }
    /// The smallest rect containing both this and `other`.
    [[nodiscard]] HeightRect merged(const HeightRect& other) const;
};

/// Write a rect of normalized heights to everything that holds them: the
/// asset (CPU array and GPU texture) and, when a play session has built a
/// collision body for the node, that body. One call, so the two can never be
/// updated separately and drift apart.
void apply_terrain_rect(TerrainAsset& terrain, UploadContext& upload, PhysicsSubsystem* physics,
                        uint32_t node_id, const HeightRect& rect, const float* heights);

/// Write a rect of RGBA layer weights to the asset's CPU array and GPU texture.
/// Physics does not read weights yet; when surfaces do, they update here.
void apply_terrain_weights(TerrainAsset& terrain, UploadContext& upload, const HeightRect& rect,
                           const uint8_t* rgba);

/// First point where a world-space ray enters the terrain surface. Marches at
/// sample spacing and bisects the crossing, against the same bilinear height
/// the mesh shader draws, so the brush lands where the surface is seen.
[[nodiscard]] bool raycast_terrain(const TerrainAsset& terrain, const glm::vec3& terrain_origin,
                                   const glm::vec3& ray_origin, const glm::vec3& ray_dir,
                                   glm::vec3& out_hit);

/// One sculpt stroke, mouse down to mouse up, as a single undo step. Holds the
/// touched rect before and after; a step per dab would make undoing one drag
/// take dozens of presses.
class SculptStrokeCommand final : public Command {
public:
    SculptStrokeCommand(TerrainAsset& terrain, UploadContext& upload, PhysicsSubsystem* physics,
                        uint32_t node_id, HeightRect rect, std::vector<float> before,
                        std::vector<float> after);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    TerrainAsset& terrain_;
    UploadContext& upload_;
    PhysicsSubsystem* physics_;
    uint32_t node_id_;
    HeightRect rect_;
    std::vector<float> before_;
    std::vector<float> after_;
};

/// One paint stroke as a single undo step, the weight-texture twin of
/// SculptStrokeCommand.
class PaintStrokeCommand final : public Command {
public:
    PaintStrokeCommand(TerrainAsset& terrain, UploadContext& upload, HeightRect rect,
                       std::vector<uint8_t> before, std::vector<uint8_t> after);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    TerrainAsset& terrain_;
    UploadContext& upload_;
    HeightRect rect_;
    std::vector<uint8_t> before_;
    std::vector<uint8_t> after_;
};

/// The viewport's terrain brush tool: projects the mouse onto the selected
/// terrain, draws the brush there, and turns a held button into height edits
/// or layer painting with one undo entry per stroke.
class TerrainSculptTool {
public:
    /// Which brush is in hand. `none` leaves the viewport to the gizmo and
    /// picking; the sculpt brushes edit heights on drag, `paint` edits the
    /// layer weights.
    enum class BrushTool : uint8_t { none, raise, lower, smooth, flatten, noise, paint };

    struct Brush {
        float radius{8.0f};    ///< metres
        float strength{4.0f};  ///< raise, lower, noise: metres of height per second at the centre
        float rate{2.0f};      ///< smooth, flatten, paint: fraction of the remaining difference closed per second
        float hardness{0.5f};  ///< fraction of the radius held at full strength before falloff
        uint32_t paint_layer{0};  ///< paint: which layer the stroke lays down
    };

    /// What one frame of the tool needs, gathered by the viewport that owns it.
    struct Frame {
        TerrainAsset* terrain{nullptr};
        glm::vec3 terrain_origin{0.0f};  ///< the field's min corner in world space
        uint32_t node_id{0};
        glm::vec3 ray_origin{0.0f};
        glm::vec3 ray_dir{0.0f};
        bool ray_valid{false};
        bool mouse_down{false};
        bool may_begin_stroke{false};  ///< false while the press belongs to a UI item
        float dt{0.0f};
        UploadContext* upload{nullptr};
        PhysicsSubsystem* physics{nullptr};
        DebugDrawQueue* debug{nullptr};
        CommandHistory* history{nullptr};
    };

    void update(const Frame& frame);

    [[nodiscard]] Brush& brush() { return brush_; }
    [[nodiscard]] BrushTool tool() const { return tool_; }
    void set_tool(BrushTool tool) { tool_ = tool; }
    [[nodiscard]] bool active() const { return tool_ != BrushTool::none; }
    [[nodiscard]] bool stroking() const { return stroke_active_; }

private:
    void begin_stroke(const Frame& frame);
    void dab(const Frame& frame, const glm::vec3& hit_world);
    void sculpt_dab(const Frame& frame, const glm::vec3& hit_world);
    void paint_dab(const Frame& frame, const glm::vec3& hit_world);
    void end_stroke(const Frame& frame);
    void end_sculpt_stroke(const Frame& frame);
    void end_paint_stroke(const Frame& frame);
    void draw_brush(const Frame& frame, const glm::vec3& hit_world) const;
    [[nodiscard]] bool painting() const { return tool_ == BrushTool::paint; }

    Brush brush_;
    BrushTool tool_{BrushTool::none};
    bool stroke_active_{false};
    bool flatten_target_set_{false};
    float flatten_target_{0.0f};     ///< normalized height the stroke flattens toward
    uint32_t stroke_seed_{0};        ///< varies the noise brush between strokes
    std::vector<float> neighbourhood_;  ///< dab rect plus a one-sample border, for smoothing
    bool mouse_was_down_{false};
    std::vector<float> snapshot_;  ///< the whole height field at stroke start, for undo
    HeightRect touched_;           ///< union of every dab's rect this stroke
    std::vector<float> scratch_;   ///< working buffer for the dab under edit
    std::vector<uint8_t> weight_snapshot_;  ///< the whole weight grid at stroke start
    std::vector<uint8_t> weight_scratch_;   ///< RGBA working buffer for a paint dab
};

} // namespace fjell
