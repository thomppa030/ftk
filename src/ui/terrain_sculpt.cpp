#include "ui/terrain_sculpt.hpp"

#include "core/command_history.hpp"
#include "physics/physics_subsystem.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "scripting/debug_draw_queue.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace fjell {

namespace {

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

void apply_terrain_rect(TerrainAsset& terrain, UploadContext& upload, PhysicsSubsystem* physics,
                        uint32_t node_id, const HeightRect& rect, const float* heights) {
    terrain.apply_heights(rect.x, rect.z, rect.width, rect.height, heights, upload);
    if (physics == nullptr) return;
    if (auto* world = physics->world()) {
        world->update_terrain_heights(node_id, rect.x, rect.z, rect.width, rect.height, heights);
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

SculptStrokeCommand::SculptStrokeCommand(TerrainAsset& terrain, UploadContext& upload,
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

void TerrainSculptTool::update(const Frame& frame) {
    const bool pressed = frame.mouse_down && !mouse_was_down_;
    mouse_was_down_ = frame.mouse_down;

    if (!active() || frame.terrain == nullptr || frame.upload == nullptr) {
        if (stroke_active_) end_stroke(frame);
        return;
    }

    glm::vec3 hit{0.0f};
    const bool hovering = frame.ray_valid && raycast_terrain(*frame.terrain, frame.terrain_origin,
                                                             frame.ray_origin, frame.ray_dir, hit);

    if (stroke_active_ && !frame.mouse_down) {
        end_stroke(frame);
    }
    if (!stroke_active_ && pressed && frame.may_begin_stroke && hovering) {
        begin_stroke(frame);
    }
    if (stroke_active_ && hovering) {
        dab(frame, hit);
    }
    if (hovering) {
        draw_brush(frame, hit);
    }
}

void TerrainSculptTool::begin_stroke(const Frame& frame) {
    // One copy of the whole field is far simpler than tracking which cells a
    // stroke has already captured, and a few megabytes per click is nothing.
    snapshot_ = frame.terrain->data().heights;
    touched_ = {};
    flatten_target_set_ = false;
    ++stroke_seed_;
    stroke_active_ = true;
}

void TerrainSculptTool::dab(const Frame& frame, const glm::vec3& hit_world) {
    TerrainAsset& terrain = *frame.terrain;
    const uint32_t samples = terrain.sample_count();
    if (samples < 2 || terrain.height_scale() <= 0.0f) return;
    const float spacing = terrain.world_size() / static_cast<float>(samples - 1);

    const float cx = (hit_world.x - frame.terrain_origin.x) / spacing;
    const float cz = (hit_world.z - frame.terrain_origin.z) / spacing;
    const float radius = std::max(brush_.radius / spacing, 0.5f);
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
    // Rate brushes close a fraction of the remaining difference per second.
    const float amount = (brush_.strength * frame.dt) / terrain.height_scale();
    const float blend = 1.0f - std::exp(-brush_.rate * frame.dt);
    const float hard = std::clamp(brush_.hardness, 0.0f, 0.999f);

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
            const float d = std::sqrt((dx * dx) + (dz * dz));
            if (d >= 1.0f) continue;
            float weight = 1.0f;
            if (d > hard) {
                const float t = (d - hard) / (1.0f - hard);
                weight = 1.0f - (t * t * (3.0f - (2.0f * t)));
            }

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
            case BrushTool::none:    break;
            }
        }
    }

    apply_terrain_rect(terrain, *frame.upload, frame.physics, frame.node_id, rect,
                       scratch_.data());
    touched_ = touched_.merged(rect);
}

void TerrainSculptTool::end_stroke(const Frame& frame) {
    stroke_active_ = false;
    if (touched_.empty() || frame.history == nullptr || frame.terrain == nullptr) {
        snapshot_.clear();
        return;
    }

    const uint32_t samples = frame.terrain->sample_count();
    std::vector<float> before(touched_.count());
    for (uint32_t row = 0; row < touched_.height; ++row) {
        const float* src = &snapshot_[(static_cast<size_t>(touched_.z + row) * samples) + touched_.x];
        std::copy(src, src + touched_.width, &before[static_cast<size_t>(row) * touched_.width]);
    }
    std::vector<float> after(touched_.count());
    frame.terrain->copy_heights(touched_.x, touched_.z, touched_.width, touched_.height,
                                after.data());
    snapshot_.clear();

    // The stroke has already been applied live; the history's execute() call
    // re-applies the same data once, which is the price of one entry per stroke.
    frame.history->execute(std::make_unique<SculptStrokeCommand>(
        *frame.terrain, *frame.upload, frame.physics, frame.node_id, touched_,
        std::move(before), std::move(after)));
}

void TerrainSculptTool::draw_brush(const Frame& frame, const glm::vec3& hit_world) const {
    if (frame.debug == nullptr) return;
    constexpr int SEGMENTS = 48;
    constexpr float LIFT = 0.1f;  // keep the ring from z-fighting the ground
    const glm::vec4 colour = stroke_active_ ? glm::vec4{1.0f, 0.6f, 0.15f, 1.0f}
                                            : glm::vec4{0.3f, 1.0f, 0.4f, 1.0f};
    const TerrainAsset& terrain = *frame.terrain;

    auto on_surface = [&](float angle) {
        const glm::vec3 p{hit_world.x + (std::cos(angle) * brush_.radius), 0.0f,
                          hit_world.z + (std::sin(angle) * brush_.radius)};
        const float y = frame.terrain_origin.y +
                        terrain.height_at(p.x - frame.terrain_origin.x, p.z - frame.terrain_origin.z);
        return glm::vec3{p.x, y + LIFT, p.z};
    };

    glm::vec3 prev = on_surface(0.0f);
    for (int i = 1; i <= SEGMENTS; ++i) {
        const float angle = (static_cast<float>(i) / SEGMENTS) * 6.28318530f;
        const glm::vec3 cur = on_surface(angle);
        frame.debug->line(prev, cur, colour);
        prev = cur;
    }
    frame.debug->line(hit_world + glm::vec3{0.0f, LIFT, 0.0f},
                      hit_world + glm::vec3{0.0f, LIFT + 1.0f, 0.0f}, colour);
}

} // namespace fjell
