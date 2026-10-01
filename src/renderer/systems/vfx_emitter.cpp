#include "renderer/systems/vfx_emitter.hpp"
#include "ftk/base/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace fjell {

float VfxEmitter::effective_duration() const {
    return (duration > 0.0f) ? duration : lifetime_max();
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

float VfxEmitter::emission_rate() const {
    for (const auto& m : spawn_modules) {
        if (m->type() == ModuleType::spawn_rate) {
            return static_cast<const SpawnRateModule*>(m.get())->rate;
        }
    }
    return 0.0f;
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

uint32_t VfxEmitter::write_gpu_modules(uint8_t* buffer, uint32_t max_bytes) const {
    // Modules don't expose their write size ahead of writing (write_gpu_data
    // just writes and returns the count), and sizes vary a lot — curve
    // modules run 528 bytes against ~80 for a typical module — so there's
    // no cheap way to pre-check "will this fit" before calling it. Instead,
    // write into a worst-case-sized scratch buffer, then copy only what
    // actually fits into the caller's window. A module that would overflow
    // is dropped entirely (never partially written — a half-written module
    // header would desync the GPU-side reader's walk) and every remaining
    // module after it is dropped too, since offsets would no longer be
    // contiguous with what got written.
    constexpr uint32_t SCRATCH_SIZE = 8192; // generous upper bound; see MAX_MODULE_BYTES below
    static_assert(SCRATCH_SIZE >= 2048, "scratch must cover the largest real per-emitter budget");
    std::array<uint8_t, SCRATCH_SIZE> scratch{};
    uint32_t offset = 0;
    bool dropped_any = false;

    auto try_write = [&](const std::unique_ptr<VfxModule>& m) {
        if (!m->enabled) return;
        // Terminator (8 bytes) must still fit after this module.
        if (offset + 8 > max_bytes) { dropped_any = true; return; }
        uint32_t written = m->write_gpu_data(scratch.data() + offset);
        if (offset + written + 8 > max_bytes) {
            dropped_any = true;
            return; // leave `offset` unadvanced — this module's bytes are discarded
        }
        offset += written;
    };

    for (const auto& m : spawn_modules) try_write(m);
    for (const auto& m : update_modules) try_write(m);

    if (dropped_any) {
        FJELL_CORE_ERROR("VfxEmitter '{}': module data exceeds its {}-byte budget, "
                         "one or more modules were dropped this frame", name, max_bytes);
    }

    // Terminator (type = 0xFFFFFFFF, size = 0) — always written, always fits
    // (guaranteed room by the +8 check above).
    auto* term = reinterpret_cast<GpuModuleHeader*>(scratch.data() + offset);
    term->type = 0xFFFFFFFF;
    term->size = 0;
    offset += 8;

    std::memcpy(buffer, scratch.data(), offset);
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

float VfxEffect::total_duration() const {
    float total = 0.0f;
    for (const auto& e : emitters) {
        float end = e.start_delay + e.effective_duration();
        if (end > total) total = end;
    }
    return total;
}

float VfxEffect::time_to_finish() const {
    float finish = 0.0f;
    for (const auto& e : emitters) {
        finish = std::max(finish, e.start_delay + e.effective_duration() + e.lifetime_max());
    }
    return finish;
}

void VfxEffect::reset_playback() {
    for (auto& e : emitters) {
        e.emit_accumulator = 0.0f;
        e.distance_accumulator = 0.0f;
        e.burst_elapsed = 0.0f;
        e.emitter_elapsed = 0.0f;
        e.local_elapsed = 0.0f;
        e.loops_completed = 0;
        e.burst_fired = false;
        e.paused = false;
        e.finished = false;
    }
}

} // namespace fjell
