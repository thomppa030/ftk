#pragma once

#include "core/command.hpp"
#include "ui/brush.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class CommandHistory;
class PhysicsSubsystem;
class TerrainAsset;
namespace gpu {
class Upload;
}

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
void apply_terrain_rect(TerrainAsset& terrain, gpu::Upload& upload, PhysicsSubsystem* physics,
                        uint32_t node_id, const HeightRect& rect, const float* heights);

/// Write a rect of RGBA layer weights to everything that holds them: the
/// asset (CPU array and GPU texture) and, when a play session has built a
/// collision body for the node, that body's per-quad surfaces. `rect` is in
/// weight texels.
void apply_terrain_weights(TerrainAsset& terrain, gpu::Upload& upload,
                           PhysicsSubsystem* physics, uint32_t node_id, const HeightRect& rect,
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
    SculptStrokeCommand(TerrainAsset& terrain, gpu::Upload& upload, PhysicsSubsystem* physics,
                        uint32_t node_id, HeightRect rect, std::vector<float> before,
                        std::vector<float> after);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    TerrainAsset& terrain_;
    gpu::Upload& upload_;
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
    PaintStrokeCommand(TerrainAsset& terrain, gpu::Upload& upload, PhysicsSubsystem* physics,
                       uint32_t node_id, HeightRect rect, std::vector<uint8_t> before,
                       std::vector<uint8_t> after);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

private:
    TerrainAsset& terrain_;
    gpu::Upload& upload_;
    PhysicsSubsystem* physics_;
    uint32_t node_id_;
    HeightRect rect_;
    std::vector<uint8_t> before_;
    std::vector<uint8_t> after_;
};

/// The terrain's brush tools: what a stroke of the viewport brush does to the
/// selected terrain — height edits or layer painting — with one undo entry
/// per stroke. The brush itself (mouse, stroke, cursor, shortcuts) is the
/// shared ViewportBrush this tool drives.
class TerrainSculptTool final : public BrushTarget {
public:
    /// Which operation is in hand. `none` leaves the viewport to the gizmo and
    /// picking; the sculpt operations edit heights on drag, `paint` edits the
    /// layer weights.
    enum class BrushTool : uint8_t { none, raise, lower, smooth, flatten, noise, paint };

    /// What one frame of the tool needs, gathered by the viewport that owns it.
    struct Frame {
        TerrainAsset* terrain{nullptr};
        glm::vec3 terrain_origin{0.0f};  ///< the field's min corner in world space
        uint32_t node_id{0};
        ViewportBrush::Frame brush;
        gpu::Upload* upload{nullptr};
        PhysicsSubsystem* physics{nullptr};
        CommandHistory* history{nullptr};
    };

    explicit TerrainSculptTool(ViewportBrush& brush) : brush_{brush} {}

    void update(const Frame& frame);

    [[nodiscard]] ViewportBrush& brush() { return brush_; }
    [[nodiscard]] BrushTool tool() const { return tool_; }
    /// Ignored while a stroke is in progress: the operation a stroke started
    /// with is the one it finishes with, and its undo entry is written from
    /// that. Ask `stroking()` first to grey the buttons out.
    void set_tool(BrushTool tool);
    [[nodiscard]] bool active() const { return tool_ != BrushTool::none; }
    [[nodiscard]] bool stroking() const { return brush_.stroking(); }

    /// Paint only: which layer the stroke lays down.
    [[nodiscard]] uint32_t paint_layer() const { return paint_layer_; }
    void set_paint_layer(uint32_t layer) { paint_layer_ = layer; }

    // BrushTarget
    bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                 glm::vec3& out_hit) const override;
    float surface_height(float x, float z) const override;
    void begin_stroke(const BrushHit& hit) override;
    void dab(const BrushHit& hit, float dt) override;
    void end_stroke() override;

private:
    void sculpt_dab(const glm::vec3& hit_world, float dt);
    void paint_dab(const glm::vec3& hit_world, float dt);
    void end_sculpt_stroke();
    void end_paint_stroke();
    /// Whether the tool in hand paints. A stroke asks `stroke_.painting`
    /// instead, so switching tools cannot send it down the other branch.
    [[nodiscard]] bool painting() const { return tool_ == BrushTool::paint; }

    /// What a stroke needs to write its undo entry, taken at begin_stroke.
    /// end_stroke can be called after the frame is gone — a tool change or a
    /// deselect under the brush — so it must not read `frame_`.
    struct StrokeContext {
        TerrainAsset* terrain{nullptr};
        gpu::Upload* upload{nullptr};
        PhysicsSubsystem* physics{nullptr};
        CommandHistory* history{nullptr};
        uint32_t node_id{0};
        bool painting{false};
    };

    ViewportBrush& brush_;
    const Frame* frame_{nullptr};  ///< the frame being updated, for the BrushTarget calls
    StrokeContext stroke_;         ///< valid between begin_stroke and end_stroke
    BrushTool tool_{BrushTool::none};
    uint32_t paint_layer_{0};
    bool flatten_target_set_{false};
    float flatten_target_{0.0f};     ///< normalized height the stroke flattens toward
    uint32_t stroke_seed_{0};        ///< varies the noise brush between strokes
    std::vector<float> neighbourhood_;  ///< dab rect plus a one-sample border, for smoothing
    std::vector<float> snapshot_;  ///< the whole height field at stroke start, for undo
    HeightRect touched_;           ///< union of every dab's rect this stroke
    std::vector<float> scratch_;   ///< working buffer for the dab under edit
    std::vector<uint8_t> weight_snapshot_;  ///< the whole weight grid at stroke start
    std::vector<uint8_t> weight_scratch_;   ///< RGBA working buffer for a paint dab
};

} // namespace fjell
