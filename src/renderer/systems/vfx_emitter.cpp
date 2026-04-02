#include "renderer/systems/vfx_emitter.hpp"

namespace fjell {

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

float VfxEmitter::emission_rate() const {
    for (const auto& m : spawn_modules) {
        if (m->type() == ModuleType::spawn_rate) {
            return static_cast<const SpawnRateModule*>(m.get())->rate;
        }
    }
    return 50.0f;
}

uint32_t VfxEmitter::burst_count() const {
    for (const auto& m : spawn_modules) {
        if (m->type() == ModuleType::burst) {
            return static_cast<const BurstModule*>(m.get())->count;
        }
    }
    return 0;
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
    // Upper bound: largest module is InitialVelocity at 72 bytes.
    // Use 80 bytes per module for safe alignment headroom.
    uint32_t count = static_cast<uint32_t>(spawn_modules.size() + update_modules.size());
    return count * 80 + 8; // +8 for terminator
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

    // Terminator (type = 0xFFFFFFFF, size = 0) — always written
    auto* term = reinterpret_cast<GpuModuleHeader*>(buffer + offset);
    term->type = 0xFFFFFFFF;
    term->size = 0;
    offset += 8;

    return offset;
}

// ── VfxEffect ────────────────────────────────────────────────────────────

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
