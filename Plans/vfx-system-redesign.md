# VFX System Redesign — Niagara-lite

## Philosophy

Module stack per emitter. Each module is a small unit that reads/writes particle attributes in order. The editor is a stack view (not a free-form graph). Modules are data-driven — the sim shader dispatches a module list from a buffer, not hardcoded logic.

## Current state

GPU compute particles with fixed attributes (pos, vel, color, size, lifetime). Flat config struct (`EmitterComponent`), imnodes graph that's really just a config panel. Works but not extensible.

## Core architecture

### Particle attributes

Particles have a fixed base layout (position, velocity, lifetime) plus optional attributes declared by modules. All stored in a single SSBO per emitter.

Base (always present):
- `vec3 position`
- `float lifetime`
- `vec3 velocity`
- `float max_lifetime`

Extended (added by modules that need them):
- `vec4 color` — ColorOverLife, InitialColor
- `float size` — SizeOverLife, InitialSize
- `float size_end` — SizeOverLife
- `uint emitter_index` — always present (for mega-buffer addressing)
- `float noise_phase` — Noise/Turbulence
- `vec3 initial_velocity` — VelocityOverLife (needs reference)
- `float rotation` — RotateOverLife, InitialRotation
- `float angular_velocity` — InitialAngularVelocity

For now: keep the fixed 64-byte particle struct. Extensible attributes are a v2 concern. The module system doesn't require dynamic attributes to be useful — modules just read/write the fixed fields.

### Module types

Three categories, executed in order within each:

**Spawn modules** (run once when particle is born):
- `SpawnRate` — particles per second
- `SpawnBurst` — emit N particles at time T
- `InitialPosition` — point, sphere, box, cone, ring, mesh surface
- `InitialVelocity` — directional, random, radial, cone
- `InitialColor` — fixed or random range
- `InitialSize` — fixed or random range
- `InitialLifetime` — random range
- `InitialRotation` — random range

**Update modules** (run every frame for alive particles):
- `Gravity` — constant acceleration
- `Drag` — velocity damping
- `Noise` — curl noise on position or velocity
- `Attract` — point attractor with falloff
- `ColorOverLife` — lerp or curve
- `SizeOverLife` — lerp or curve
- `VelocityOverLife` — scale or curve
- `RotateOverLife` — angular velocity
- `Collision` — plane or depth buffer, bounce/kill
- `VelocityLimit` — clamp speed

**Render modules** (configure how particles draw):
- `Billboard` — camera-facing quad (default)
- `StretchedBillboard` — velocity-aligned
- `MeshParticle` — instanced mesh
- `SpriteSheet` — atlas UV animation
- `SoftParticle` — depth fade alpha
- `Trail` — ribbon between consecutive particles

### Module data format

Each module is a tagged struct in a flat buffer:

```
struct ModuleHeader {
    uint32_t type;      // enum ModuleType
    uint32_t size;      // total size including header
};
// followed by module-specific params
```

The sim shader reads the module list and dispatches:
```glsl
uint offset = 0;
while (offset < module_buffer_size) {
    uint type = module_data[offset];
    uint size = module_data[offset + 1];
    switch (type) {
        case MOD_GRAVITY: apply_gravity(particle, offset); break;
        case MOD_COLOR_OVER_LIFE: apply_color_over_life(particle, offset); break;
        // ...
    }
    offset += size;
}
```

### Emitter definition

```cpp
struct VfxEmitter {
    std::string name;
    uint32_t max_particles{10000};
    ParticleBlendMode blend_mode{ParticleBlendMode::additive};

    // Ordered module stacks
    std::vector<std::unique_ptr<VfxModule>> spawn_modules;
    std::vector<std::unique_ptr<VfxModule>> update_modules;
    std::vector<std::unique_ptr<VfxModule>> render_modules;
};
```

### VfxModule base

```cpp
struct VfxModule {
    virtual ~VfxModule() = default;
    virtual ModuleType type() const = 0;
    virtual const char* name() const = 0;

    // Serialize params to GPU buffer (returns bytes written)
    virtual uint32_t write_gpu_data(uint8_t* buffer) const = 0;

    // Draw ImGui inspector for this module's parameters
    virtual bool draw_inspector() = 0;

    // Serialize to/from JSON
    virtual void to_json(nlohmann::json& j) const = 0;
    virtual void from_json(const nlohmann::json& j) = 0;

    bool enabled{true};
};
```

### .fjvfx format (v2)

```json
{
    "version": 2,
    "emitters": [
        {
            "name": "sparks",
            "max_particles": 5000,
            "blend_mode": "additive",
            "spawn": [
                { "type": "SpawnRate", "rate": 100.0 },
                { "type": "InitialPosition", "shape": "sphere", "radius": 0.1 },
                { "type": "InitialVelocity", "mode": "cone", "direction": [0,1,0], "angle": 30, "speed_min": 2.0, "speed_max": 5.0 },
                { "type": "InitialColor", "color": [1.0, 0.8, 0.2, 1.0] },
                { "type": "InitialSize", "min": 0.05, "max": 0.1 },
                { "type": "InitialLifetime", "min": 0.5, "max": 1.5 }
            ],
            "update": [
                { "type": "Gravity", "force": [0, -9.81, 0] },
                { "type": "ColorOverLife", "start": [1,0.8,0.2,1], "end": [1,0.1,0,0] },
                { "type": "SizeOverLife", "start": 0.1, "end": 0.02 }
            ],
            "render": [
                { "type": "Billboard" }
            ]
        }
    ]
}
```

## Implementation phases

### Phase 1 — Module system foundation
- `VfxModule` base class + concrete modules (SpawnRate, InitialPosition point, InitialVelocity directional, InitialColor, InitialSize, InitialLifetime, Gravity, ColorOverLife, SizeOverLife, Billboard)
- `VfxEmitter` with module stacks
- Module buffer serialization (write to GPU buffer)
- Refactor `particle_sim.comp` to read module buffer instead of hardcoded `GpuEmitterData`
- Refactor `particle_emit.comp` to read spawn modules from buffer
- `.fjvfx` v2 load/save
- Migrate existing `EmitterComponent` to use new system
- Keep backward compat with existing `.fjvfx` v1 files (flat config → auto-convert to module stack)

### Phase 2 — Spawn shapes + new modules
- InitialPosition: sphere, box, cone, ring
- InitialVelocity: radial, cone, random
- Drag, Noise/Turbulence, Attract
- VelocityLimit
- RotateOverLife, InitialRotation

### Phase 3 — VFX Editor Context
- Stack-based editor UI (not node graph)
- Module palette: drag modules into spawn/update/render stacks
- Per-module inspector (uses VfxModule::draw_inspector())
- Preview viewport with isolated emitter
- Restart, time scale, warm-up controls
- Open .fjvfx from content browser

### Phase 4 — Rendering improvements
- StretchedBillboard, SoftParticle
- SpriteSheet (atlas UV animation)
- MeshParticle (instanced mesh per particle)
- Trail/ribbon rendering
- GPU sorting for correct transparency

### Phase 5 — Advanced
- Sub-emitters (child emitters triggered on death/collision)
- Collision (plane, depth buffer)
- Multi-emitter effects
- Light emission (particles in forward+ grid)
- Curves (replace linear lerp with keyframe curves for color/size/velocity over life)
