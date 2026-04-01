#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/components/particle_emitter.hpp"

namespace fjell {

void EmitterComponent::ensure_effect() {
    if (!effect) {
        effect = std::make_shared<VfxEffect>(VfxEffect::from_legacy(*this));
    }
}

VfxEmitter VfxEmitter::make_default() {
    VfxEmitter e;
    e.spawn_modules.push_back(std::make_unique<SpawnRateModule>());
    e.spawn_modules.push_back(std::make_unique<InitialLifetimeModule>());
    e.spawn_modules.push_back(std::make_unique<InitialPositionModule>());
    e.spawn_modules.push_back(std::make_unique<InitialVelocityModule>());
    e.spawn_modules.push_back(std::make_unique<InitialColorModule>());
    e.spawn_modules.push_back(std::make_unique<InitialSizeModule>());
    e.update_modules.push_back(std::make_unique<GravityModule>());
    e.update_modules.push_back(std::make_unique<ColorOverLifeModule>());
    e.update_modules.push_back(std::make_unique<SizeOverLifeModule>());
    e.render_modules.push_back(std::make_unique<BillboardModule>());
    return e;
}

VfxEmitter VfxEmitter::from_legacy(const EmitterComponent& ec) {
    VfxEmitter e;
    e.max_particles = ec.max_particles;
    e.blend_mode = ec.blend_mode;

    auto spawn_rate = std::make_unique<SpawnRateModule>();
    spawn_rate->rate = ec.emission_rate;
    e.spawn_modules.push_back(std::move(spawn_rate));

    auto lifetime = std::make_unique<InitialLifetimeModule>();
    lifetime->lifetime_min = ec.lifetime_min;
    lifetime->lifetime_max = ec.lifetime_max;
    e.spawn_modules.push_back(std::move(lifetime));

    e.spawn_modules.push_back(std::make_unique<InitialPositionModule>()); // point

    auto velocity = std::make_unique<InitialVelocityModule>();
    velocity->velocity_min = ec.velocity_min;
    velocity->velocity_max = ec.velocity_max;
    e.spawn_modules.push_back(std::move(velocity));

    auto color = std::make_unique<InitialColorModule>();
    color->color = ec.color_start;
    e.spawn_modules.push_back(std::move(color));

    auto size = std::make_unique<InitialSizeModule>();
    size->size_min = ec.size_start;
    size->size_max = ec.size_start; // legacy had single start value
    e.spawn_modules.push_back(std::move(size));

    auto gravity = std::make_unique<GravityModule>();
    gravity->force = ec.gravity;
    e.update_modules.push_back(std::move(gravity));

    auto col_life = std::make_unique<ColorOverLifeModule>();
    col_life->color_start = ec.color_start;
    col_life->color_end = ec.color_end;
    e.update_modules.push_back(std::move(col_life));

    auto size_life = std::make_unique<SizeOverLifeModule>();
    size_life->size_start = ec.size_start;
    size_life->size_end = ec.size_end;
    e.update_modules.push_back(std::move(size_life));

    e.render_modules.push_back(std::make_unique<BillboardModule>());
    return e;
}

float VfxEmitter::emission_rate() const {
    for (const auto& m : spawn_modules) {
        if (m->type() == ModuleType::spawn_rate) {
            return static_cast<const SpawnRateModule*>(m.get())->rate;
        }
    }
    return 50.0f;
}

float VfxEmitter::lifetime_min() const {
    for (const auto& m : spawn_modules) {
        if (m->type() == ModuleType::initial_lifetime) {
            return static_cast<const InitialLifetimeModule*>(m.get())->lifetime_min;
        }
    }
    return 1.0f;
}

float VfxEmitter::lifetime_max() const {
    for (const auto& m : spawn_modules) {
        if (m->type() == ModuleType::initial_lifetime) {
            return static_cast<const InitialLifetimeModule*>(m.get())->lifetime_max;
        }
    }
    return 3.0f;
}

uint32_t VfxEmitter::gpu_data_size() const {
    // Estimate: each module writes header (8 bytes) + up to 48 bytes of params
    // Conservative upper bound per module: 64 bytes
    uint32_t count = static_cast<uint32_t>(spawn_modules.size() + update_modules.size());
    return count * 64 + 8; // +8 for sentinel/terminator
}

uint32_t VfxEmitter::write_gpu_modules(uint8_t* buffer) const {
    uint32_t offset = 0;

    // Write spawn modules
    for (const auto& m : spawn_modules) {
        if (!m->enabled) continue;
        uint32_t written = m->write_gpu_data(buffer + offset);
        offset += written;
    }

    // Write update modules
    for (const auto& m : update_modules) {
        if (!m->enabled) continue;
        uint32_t written = m->write_gpu_data(buffer + offset);
        offset += written;
    }

    // Terminator (type = 0xFFFFFFFF, size = 0)
    if (offset + 8 <= gpu_data_size()) {
        auto* term = reinterpret_cast<GpuModuleHeader*>(buffer + offset);
        term->type = 0xFFFFFFFF;
        term->size = 0;
        offset += 8;
    }

    return offset;
}

// ── VfxEffect ────────────────────────────────────────────────────────────

VfxEffect VfxEffect::from_legacy(const EmitterComponent& ec) {
    VfxEffect effect;
    effect.name = "effect";
    effect.emitters.push_back(VfxEmitter::from_legacy(ec));
    return effect;
}

uint32_t VfxEffect::total_max_particles() const {
    uint32_t total = 0;
    for (const auto& e : emitters) total += e.max_particles;
    return total;
}

float VfxEffect::max_lifetime() const {
    float max_lt = 0.0f;
    for (const auto& e : emitters) {
        float lt = e.lifetime_max();
        if (lt > max_lt) max_lt = lt;
    }
    return max_lt;
}

} // namespace fjell
