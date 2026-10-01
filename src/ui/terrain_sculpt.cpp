#include "ui/terrain_sculpt.hpp"

#include "ftk/app/command_history.hpp"
#include "physics/physics_subsystem.hpp"
#include "renderer/resources/terrain_asset.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace fjell {

namespace {

// What full brush strength means for each operation, per second at the
// centre of the brush. The shared strength knob is 0..1; these are its
// units.
constexpr float RAISE_METRES_PER_SECOND = 10.0f;   // raise, lower, noise
constexpr float BLEND_RATE_PER_SECOND = 10.0f;     // smooth, flatten: fraction of the gap closed
constexpr float PAINT_COVERAGE_PER_SECOND = 4.0f;  // paint: full coverage takes 1/this seconds

// Stable per-sample noise in [-1, 1]. The seed changes per stroke so two
// strokes over the same ground do not stack the same pattern.
float sample_noise(uint32_t x, uint32_t z, uint32_t seed) {
    uint32_t h = seed;
    h ^= x * 0x8DA6B343U;
    h ^= z * 0xD8163841U;
    h ^= h >> 15;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12;
    h *= 0x297A2D39U;
    h ^= h >> 15;
    return (static_cast<float>(h & 0xFFFFFFU) / static_cast<float>(0x1000000U)) * 2.0f - 1.0f;
}

} // namespace

HeightRect HeightRect::merged(const HeightRect& other) const {
    if (empty()) return other;
    if (other.empty()) return *this;
    const uint32_t x0 = std::min(x, other.x);
    const uint32_t z0 = std::min(z, other.z);
    const uint32_t x1 = std::max(x + width, other.x + other.width);
    const uint32_t z1 = std::max(z + height, other.z + other.height);
    return {x0, z0, x1 - x0, z1 - z0};
}

void apply_terrain_rect(TerrainAsset& terrain, ftk::gpu::Upload& upload, PhysicsSubsystem* physics,
                        uint32_t node_id, const HeightRect& rect, const float* heights) {
    terrain.apply_heights(rect.x, rect.z, rect.width, rect.height, heights, upload);
    if (physics == nullptr) return;
    if (auto* world = physics->world()) {
        world->update_terrain_heights(node_id, rect.x, rect.z, rect.width, rect.height, heights);
    }
}

void apply_terrain_weights(TerrainAsset& terrain, ftk::gpu::Upload& upload,
                           PhysicsSubsystem* physics, uint32_t node_id, const HeightRect& rect,
                           const uint8_t* rgba) {
    terrain.apply_weights(rect.x, rect.z, rect.width, rect.height, rgba, upload);
    if (physics == nullptr) return;
    if (auto* world = physics->world()) {
        world->update_terrain_surfaces(node_id, terrain, rect.x, rect.z, rect.width, rect.height);
    }
}

bool raycast_terrain(const TerrainAsset& terrain, const glm::vec3& terrain_origin,
                     const glm::vec3& ray_origin, const glm::vec3& ray_dir, glm::vec3& out_hit) {
    const float size = terrain.world_size();
    const uint32_t samples = terrain.sample_count();
    if (samples < 2 || size <= 0.0f) return false;
    const float spacing = size / static_cast<float>(samples - 1);

    // Clip the ray to the box the field can occupy, so the march starts at the
    // terrain instead of at the camera.
    const glm::vec3 lo{terrain_origin.x, terrain_origin.y + terrain.min_world_height(),
                       terrain_origin.z};
    const glm::vec3 hi{terrain_origin.x + size, terrain_origin.y + terrain.max_world_height(),
                       terrain_origin.z + size};
    float t_enter = 0.0f;
    float t_exit = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const float o = ray_origin[axis];
        const float d = ray_dir[axis];
        if (std::abs(d) < 1e-8f) {
            if (o < lo[axis] || o > hi[axis]) return false;
            continue;
        }
        float t0 = (lo[axis] - o) / d;
        float t1 = (hi[axis] - o) / d;
        if (t0 > t1) std::swap(t0, t1);
        t_enter = std::max(t_enter, t0);
        t_exit = std::min(t_exit, t1);
        if (t_enter > t_exit) return false;
    }

    // Signed height of the ray above the surface at parameter t.
    auto gap_at = [&](float t) {
        const glm::vec3 p = ray_origin + ray_dir * t;
        const float surface = terrain_origin.y +
                              terrain.height_at(p.x - terrain_origin.x, p.z - terrain_origin.z);
        return p.y - surface;
    };

    float prev_t = t_enter;
    bool prev_above = gap_at(prev_t) > 0.0f;
    for (float t = t_enter + spacing;; t += spacing) {
        const float tc = std::min(t, t_exit);
        const bool above = gap_at(tc) > 0.0f;
        if (prev_above && !above) {
            float a = prev_t;
            float b = tc;
            for (int i = 0; i < 8; ++i) {
                const float m = 0.5f * (a + b);
                if (gap_at(m) > 0.0f) a = m; else b = m;
            }
            out_hit = ray_origin + ray_dir * (0.5f * (a + b));
            return true;
        }
        prev_above = above;
        prev_t = tc;
        if (tc >= t_exit) break;
    }
    return false;
}

SculptStrokeCommand::SculptStrokeCommand(TerrainAsset& terrain, ftk::gpu::Upload& upload,
                                         PhysicsSubsystem* physics, uint32_t node_id,
                                         HeightRect rect, std::vector<float> before,
                                         std::vector<float> after)
    : terrain_{terrain}, upload_{upload}, physics_{physics}, node_id_{node_id}, rect_{rect},
      before_{std::move(before)}, after_{std::move(after)} {}

void SculptStrokeCommand::execute() {
    apply_terrain_rect(terrain_, upload_, physics_, node_id_, rect_, after_.data());
}

void SculptStrokeCommand::undo() {
    apply_terrain_rect(terrain_, upload_, physics_, node_id_, rect_, before_.data());
}

std::string SculptStrokeCommand::description() const {
    return "Sculpt terrain";
}

PaintStrokeCommand::PaintStrokeCommand(TerrainAsset& terrain, ftk::gpu::Upload& upload,
                                       PhysicsSubsystem* physics, uint32_t node_id,
                                       HeightRect rect, std::vector<uint8_t> before,
                                       std::vector<uint8_t> after)
    : terrain_{terrain}, upload_{upload}, physics_{physics}, node_id_{node_id}, rect_{rect},
      before_{std::move(before)}, after_{std::move(after)} {}

void PaintStrokeCommand::execute() {
    apply_terrain_weights(terrain_, upload_, physics_, node_id_, rect_, after_.data());
}

void PaintStrokeCommand::undo() {
    apply_terrain_weights(terrain_, upload_, physics_, node_id_, rect_, before_.data());
}

std::string PaintStrokeCommand::description() const {
    return "Paint terrain";
}

// ── TerrainSculptTool ──

void TerrainSculptTool::set_tool(BrushTool tool) {
    // A stroke keeps the operation it began with. Switching under a held
    // button would end the stroke from outside the frame and hand the undo
    // entry the wrong snapshot.
    if (brush_.stroking()) return;
    tool_ = tool;
    // Green sculpts, blue paints; the brush shows which is in hand.
    brush_.set_cursor_colour(painting() ? glm::vec4{0.4f, 0.65f, 1.0f, 1.0f}
                                        : glm::vec4{0.3f, 1.0f, 0.4f, 1.0f});
}

void TerrainSculptTool::update(const Frame& frame) {
    if (!active() || frame.terrain == nullptr || frame.upload == nullptr) {
        // The stroke still lands in history: cancel ends it through the
        // context it captured, not through this frame.
        brush_.cancel(*this);
        return;
    }
    frame_ = &frame;
    brush_.update(frame.brush, *this);
    frame_ = nullptr;
}

bool TerrainSculptTool::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                glm::vec3& out_hit) const {
    if (frame_ == nullptr || frame_->terrain == nullptr) return false;
    return raycast_terrain(*frame_->terrain, frame_->terrain_origin, origin, direction, out_hit);
}

float TerrainSculptTool::surface_height(float x, float z) const {
    if (frame_ == nullptr || frame_->terrain == nullptr) return 0.0f;
    const glm::vec3& origin = frame_->terrain_origin;
    return origin.y + frame_->terrain->height_at(x - origin.x, z - origin.z);
}

void TerrainSculptTool::begin_stroke(const BrushHit& /*hit*/) {
    stroke_ = {frame_->terrain, frame_->upload, frame_->physics,
               frame_->history, frame_->node_id, painting()};

    // One copy of the whole grid is far simpler than tracking which cells a
    // stroke has already captured, and a few megabytes per click is nothing.
    TerrainAsset& terrain = *frame_->terrain;
    if (stroke_.painting) {
        const uint32_t side = terrain.weight_count();
        weight_snapshot_.resize(static_cast<size_t>(side) * side * 4);
        terrain.copy_weights(0, 0, side, side, weight_snapshot_.data());
    } else {
        snapshot_ = terrain.data().heights;
    }
    touched_ = {};
    flatten_target_set_ = false;
    ++stroke_seed_;
}

void TerrainSculptTool::dab(const BrushHit& hit, float dt) {
    if (stroke_.painting) {
        paint_dab(hit.position, dt);
    } else {
        sculpt_dab(hit.position, dt);
    }
}

// Weight texels cover the field edge to edge, each centred at (i + 0.5) / N,
// which is where the sampler reads them from the 0..1 terrain UV.
void TerrainSculptTool::paint_dab(const glm::vec3& hit_world, float dt) {
    TerrainAsset& terrain = *frame_->terrain;
    const BrushSettings& settings = brush_.settings();
    const uint32_t side = terrain.weight_count();
    const float size = terrain.world_size();
    if (side == 0 || size <= 0.0f || paint_layer_ >= 4) return;
    const float texels_per_metre = static_cast<float>(side) / size;

    const float cx = ((hit_world.x - frame_->terrain_origin.x) * texels_per_metre) - 0.5f;
    const float cz = ((hit_world.z - frame_->terrain_origin.z) * texels_per_metre) - 0.5f;
    const float radius = std::max(settings.radius * texels_per_metre, 0.5f);
    const int last = static_cast<int>(side) - 1;
    const int x0 = std::clamp(static_cast<int>(std::floor(cx - radius)), 0, last);
    const int z0 = std::clamp(static_cast<int>(std::floor(cz - radius)), 0, last);
    const int x1 = std::clamp(static_cast<int>(std::ceil(cx + radius)), 0, last);
    const int z1 = std::clamp(static_cast<int>(std::ceil(cz + radius)), 0, last);
    if (x1 < x0 || z1 < z0) return;

    const HeightRect rect{static_cast<uint32_t>(x0), static_cast<uint32_t>(z0),
                          static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(z1 - z0 + 1)};
    weight_scratch_.resize(rect.count() * 4);
    terrain.copy_weights(rect.x, rect.z, rect.width, rect.height, weight_scratch_.data());

    // A fixed step per second, not an exponential approach: the latter never
    // quite arrives, and a texel that is 97% rock still shows grass.
    const float step = 255.0f * settings.strength * PAINT_COVERAGE_PER_SECOND * dt;

    for (uint32_t row = 0; row < rect.height; ++row) {
        const float dz = (static_cast<float>(rect.z + row) - cz) / radius;
        for (uint32_t col = 0; col < rect.width; ++col) {
            const float dx = (static_cast<float>(rect.x + col) - cx) / radius;
            const float weight = brush_falloff(std::sqrt((dx * dx) + (dz * dz)), settings.hardness);
            if (weight <= 0.0f) continue;

            // Raise the target layer by the step and take the same amount
            // from the other layers in proportion, so the four always sum to
            // 255 and a held brush ends at exactly (255, 0, 0, 0) for its
            // layer. The blend shader divides by the sum, so the total is
            // what keeps painted ground from reading as layer 0.
            uint8_t* texel = &weight_scratch_[(static_cast<size_t>(row) * rect.width + col) * 4];
            const uint32_t target = paint_layer_;
            const float old_target = static_cast<float>(texel[target]);
            const float new_target = std::min(255.0f, old_target + (step * weight));
            float others = 0.0f;
            for (uint32_t c = 0; c < 4; ++c) {
                if (c != target) others += static_cast<float>(texel[c]);
            }
            const float remaining = 255.0f - new_target;
            const float scale = others > 0.0f ? remaining / others : 0.0f;
            for (uint32_t c = 0; c < 4; ++c) {
                const float value = c == target ? new_target
                                                : static_cast<float>(texel[c]) * scale;
                texel[c] = static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }

    apply_terrain_weights(terrain, *frame_->upload, frame_->physics, frame_->node_id, rect,
                          weight_scratch_.data());
    touched_ = touched_.merged(rect);
}

void TerrainSculptTool::sculpt_dab(const glm::vec3& hit_world, float dt) {
    TerrainAsset& terrain = *frame_->terrain;
    const BrushSettings& settings = brush_.settings();
    const uint32_t samples = terrain.sample_count();
    if (samples < 2 || terrain.height_scale() <= 0.0f) return;
    const float spacing = terrain.world_size() / static_cast<float>(samples - 1);

    const float cx = (hit_world.x - frame_->terrain_origin.x) / spacing;
    const float cz = (hit_world.z - frame_->terrain_origin.z) / spacing;
    const float radius = std::max(settings.radius / spacing, 0.5f);
    const int last = static_cast<int>(samples) - 1;
    const int x0 = std::clamp(static_cast<int>(std::floor(cx - radius)), 0, last);
    const int z0 = std::clamp(static_cast<int>(std::floor(cz - radius)), 0, last);
    const int x1 = std::clamp(static_cast<int>(std::ceil(cx + radius)), 0, last);
    const int z1 = std::clamp(static_cast<int>(std::ceil(cz + radius)), 0, last);
    if (x1 < x0 || z1 < z0) return;

    const HeightRect rect{static_cast<uint32_t>(x0), static_cast<uint32_t>(z0),
                          static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(z1 - z0 + 1)};
    scratch_.resize(rect.count());
    terrain.copy_heights(rect.x, rect.z, rect.width, rect.height, scratch_.data());

    // Strength is metres per second so the feel does not change with frame
    // rate; heights are stored normalized, so divide by the field's scale.
    // Blend operations close a fraction of the remaining difference per second.
    const float amount =
        (settings.strength * RAISE_METRES_PER_SECOND * dt) / terrain.height_scale();
    const float blend = 1.0f - std::exp(-settings.strength * BLEND_RATE_PER_SECOND * dt);

    if (tool_ == BrushTool::flatten && !flatten_target_set_) {
        flatten_target_ = terrain.height_at_sample(static_cast<uint32_t>(std::lround(cx)),
                                                   static_cast<uint32_t>(std::lround(cz)));
        flatten_target_set_ = true;
    }

    // Smoothing reads each cell's neighbours, one sample past the rect.
    const int nx0 = std::max(x0 - 1, 0);
    const int nz0 = std::max(z0 - 1, 0);
    const int nx1 = std::min(x1 + 1, last);
    const int nz1 = std::min(z1 + 1, last);
    const auto nw = static_cast<uint32_t>(nx1 - nx0 + 1);
    const auto nh = static_cast<uint32_t>(nz1 - nz0 + 1);
    if (tool_ == BrushTool::smooth) {
        neighbourhood_.resize(static_cast<size_t>(nw) * nh);
        terrain.copy_heights(static_cast<uint32_t>(nx0), static_cast<uint32_t>(nz0), nw, nh,
                             neighbourhood_.data());
    }
    auto neighbour_mean = [&](int sx, int sz) {
        float sum = 0.0f;
        for (int oz = -1; oz <= 1; ++oz) {
            const int rz = std::clamp(sz + oz, nz0, nz1) - nz0;
            for (int ox = -1; ox <= 1; ++ox) {
                const int rx = std::clamp(sx + ox, nx0, nx1) - nx0;
                sum += neighbourhood_[(static_cast<size_t>(rz) * nw) + static_cast<size_t>(rx)];
            }
        }
        return sum / 9.0f;
    };

    for (uint32_t row = 0; row < rect.height; ++row) {
        const float dz = (static_cast<float>(rect.z + row) - cz) / radius;
        for (uint32_t col = 0; col < rect.width; ++col) {
            const float dx = (static_cast<float>(rect.x + col) - cx) / radius;
            const float weight = brush_falloff(std::sqrt((dx * dx) + (dz * dz)), settings.hardness);
            if (weight <= 0.0f) continue;

            float& h = scratch_[(static_cast<size_t>(row) * rect.width) + col];
            const uint32_t sx = rect.x + col;
            const uint32_t sz = rect.z + row;
            switch (tool_) {
            case BrushTool::raise:   h += amount * weight; break;
            case BrushTool::lower:   h -= amount * weight; break;
            case BrushTool::smooth:
                h += (neighbour_mean(static_cast<int>(sx), static_cast<int>(sz)) - h) * blend * weight;
                break;
            case BrushTool::flatten: h += (flatten_target_ - h) * blend * weight; break;
            case BrushTool::noise:   h += amount * weight * sample_noise(sx, sz, stroke_seed_); break;
            case BrushTool::paint:
            case BrushTool::none:    break;
            }
        }
    }

    apply_terrain_rect(terrain, *frame_->upload, frame_->physics, frame_->node_id, rect,
                       scratch_.data());
    touched_ = touched_.merged(rect);
}

void TerrainSculptTool::end_stroke() {
    if (touched_.empty() || stroke_.history == nullptr || stroke_.terrain == nullptr ||
        stroke_.upload == nullptr) {
        snapshot_.clear();
        weight_snapshot_.clear();
        stroke_ = {};
        return;
    }
    if (stroke_.painting) {
        end_paint_stroke();
    } else {
        end_sculpt_stroke();
    }
    stroke_ = {};
}

void TerrainSculptTool::end_paint_stroke() {
    const StrokeContext& stroke = stroke_;
    const uint32_t side = stroke.terrain->weight_count();
    std::vector<uint8_t> before(touched_.count() * 4);
    for (uint32_t row = 0; row < touched_.height; ++row) {
        const uint8_t* src =
            &weight_snapshot_[((static_cast<size_t>(touched_.z + row) * side) + touched_.x) * 4];
        std::copy(src, src + (static_cast<size_t>(touched_.width) * 4),
                  &before[static_cast<size_t>(row) * touched_.width * 4]);
    }
    std::vector<uint8_t> after(touched_.count() * 4);
    stroke.terrain->copy_weights(touched_.x, touched_.z, touched_.width, touched_.height,
                                 after.data());
    weight_snapshot_.clear();

    stroke.history->execute(std::make_unique<PaintStrokeCommand>(
        *stroke.terrain, *stroke.upload, stroke.physics, stroke.node_id, touched_,
        std::move(before), std::move(after)));
}

void TerrainSculptTool::end_sculpt_stroke() {
    const StrokeContext& stroke = stroke_;
    const uint32_t samples = stroke.terrain->sample_count();
    std::vector<float> before(touched_.count());
    for (uint32_t row = 0; row < touched_.height; ++row) {
        const float* src = &snapshot_[(static_cast<size_t>(touched_.z + row) * samples) + touched_.x];
        std::copy(src, src + touched_.width, &before[static_cast<size_t>(row) * touched_.width]);
    }
    std::vector<float> after(touched_.count());
    stroke.terrain->copy_heights(touched_.x, touched_.z, touched_.width, touched_.height,
                                 after.data());
    snapshot_.clear();

    // The stroke has already been applied live; the history's execute() call
    // re-applies the same data once, which is the price of one entry per stroke.
    stroke.history->execute(std::make_unique<SculptStrokeCommand>(
        *stroke.terrain, *stroke.upload, stroke.physics, stroke.node_id, touched_,
        std::move(before), std::move(after)));
}

} // namespace fjell
