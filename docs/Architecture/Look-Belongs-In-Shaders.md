# Look Belongs in Shaders

A rule for building or reworking a visual system, and the shape of a system
that follows it. Fog is the worked example: it was rebuilt this way in
September 2026 after being the system that broke the rule, and every step
below points at what fog did.

## The rule

**What is physically true of a thing goes on its node. What it looks like
goes in a `.fjsl` with uniform hints, and the node references a material
that names that shader, never the shader itself.**

A node is geometry, physics and wiring: where the thing is, how big, which
group it belongs to, what it answers to. A shader is appearance: colour,
falloff, structure, how light meets it. The moment a look decision becomes a
C++ field on a node, it is one look forever, every further idea is another
field, another serializer entry and another inspector row, and no project can
author anything the engine did not think of.

The rule was written the day the sun node nearly got `flare_amount`,
`flare_scale` and `flare_speed` as members. It was written *because* fog
already had fifteen such fields and no project could add a wisp to the haze.

## How to tell which side a field is on

Ask what an artist in Unreal would reach for. A material means shader. A
project setting or a node property means node.

Node, because it is physical or structural:

- Position, extent, shape, edge falloff. The fog volume's transform is the
  volume; the sun's angular radius is on the sun because shafts and disc must
  agree on it.
- Anything a simulation reads. Fog's motion response, vortex strength and
  compression drive the density-field simulation the engine runs; they stay.
- Anything accumulated frame by frame. A fog bank's drift is integrated on
  the node because a heading computed from the clock tears the moment the
  wind turns it. The shader reads where the field has got to as `DRIFT`.
- Grouping and wiring: a volume's group, the material reference itself.
- Reach and cost: max distance, temporal blend, step counts.

Shader, because it is appearance:

- Colour, density profile, falloff shape, scattering, phase, anisotropy.
- Structure: noise contrast, scale, octaves, and how the structure answers
  movement. Fog's drag, curl and thinning were compute-shader constants;
  they are uniforms of the default bank material now.
- Any constant in an engine shader that an artist might want to move.

A test for a borderline field: if two nodes of the same kind would want
different *values* of it, it is a uniform; if they would want different
*behaviour* from it, it is probably a hook.

## The shape of a system that follows the rule

Every visual system in the engine that follows the rule has the same parts.
Build all of them; a system with the shader but not the material, or the
material but not the tracks, is half converted.

### 1. A shader type with a contract

A `shader_type` is a contract: which hooks a file may define, which built-ins
they see, which outputs they write. Fog's is `density()` writing `DENSITY`,
`COLOR` and `SCATTERING` from a world point, and `light()` writing
`SCATTERED` from a phase angle and a shadow. Name the type for **what it
writes**, never for how the pass samples it: fog is not `froxel`, sky is not
`cubemap`, spatial is not `fragment`.

The type lives in `fjsl_parser`'s type table and hook table, with a refusal
reason for every hook the type cannot run ("only a shader_type fog shader
writes a medium"), and a template in `shaders/include/` that the parser
splices the hooks into. Built-ins are locals or parameters in the template's
hook wrapper. A hook wrapper is a `void` function with `inout` outputs, so a
body may `return;` early, and its locals carry an engine prefix, because a
shader's uniforms are file-scope `#define`s and a uniform named `density`
would otherwise rewrite a local of that name.

Every constant that was hardcoded in the engine shader either becomes a
uniform of the default `.fjsl` or stays in the template with a comment
saying why an author cannot want to move it.

### 2. A default `.fjsl` that reproduces yesterday

The engine ships a shader for the type that generates **the same operations
in the same order** as the code it replaces, with the old fields as its
uniforms. Diff the generated GLSL against the old shader with comments
stripped before calling it done; fog's default was checked that way. A node
with no material draws with the default at its declared values, the way a
terrain or water node does.

### 3. Values in a `.fjmat`, overrides on the node

The shader is where the look's *logic* lives; the material is what a node
*references*. Most authoring never touches the shader: it picks a material
and moves its sliders. A project writes a shader when the engine's default
cannot do the look at all, a wisp field, a banded phase, and the material is
how it deploys that shader. In between, a material's shader can `use`
shader functions, so a look can be assembled from engine parts without
writing the whole hook.

Three levels, the same as the material system's:

```
.fjmat values  ←  the node's own params  ←  day profile track  ←  weather track
```

The node references a `.fjmat` by path, as terrain and water do, and holds
`params`: per-node values for individual uniforms, for the one instance that
differs from the thirty sharing the material. The inspector shows the material
field, the material's own inspector under it, then "This Node": only the
values the node holds, each with a revert, and "Override for this node" to take
another, so no uniform is shown twice. With no material, "This Node" lists
every uniform of the default shader. Changing the material clears the node's
values, which were set against the old look.

Values are `NamedValue`s (`core/named_value.hpp`), colours in sRGB as the
picker shows them, with `srgb` set so a track can scale or blend them in
linear light. The node reads its material's current values back each tick
(`material_values_`) so the resolver can seed the frame from plain data
without touching the renderer.

Do not store the values on the node instead of a material, and do not have
the node reference a `.fjsl` directly. The sky does both, and that is why
its values cannot be shared between scenes or edited as an asset; it is the
odd one out.

### 4. A private GPU row per node

The pass never writes the material's own row. It keeps a row of its own per
node, a copy of the material's that follows its textures, and writes the
frame's resolved values into that. Otherwise a track's value leaks into the
asset the inspector shows and the scene saves. Rows are taken once and kept;
the bindless registry is append-only.

### 5. Tracks by uniform name

A day profile or a weather event drives the look by naming a uniform:
`fog.<uniform>`, `fog.<group>.<uniform>`. Not two hand-picked fields. The
editors build their offers from what the scene's materials declare at draw
time, and say when a name reaches nothing. Old track names stay as aliases so
nothing authored before the rework changes.

### 6. One pipeline per shader, no permutation

Where several nodes can carry different shaders in one pass, do not compile
one engine shader per combination. Split the pass so each shader is one
ordinary pipeline: fog accumulates its medium one dispatch per source into
one image, then lights the sum once. A new material in the scene is a new
pipeline, not a rebuild of the pass. `UiRenderer`'s per-material pipelines
are the same shape.

### 7. Migration on load

A scene saved before the rework carries its look as fields. The loader turns
them into `params` on the node under the default shader's uniform names, so
the scene draws as it did and the next save writes the new shape. Nothing is
written to disk at load time and no legacy key is ever written again.

### 8. Tests and a measurement

- Parser tests for the hook rules and refusals.
- A glslc test that compiles the template with a hand-written sample naming
  every built-in, in every variant the pass compiles, so the template stays
  covered even when no shipped shader uses everything.
- The environment tests for the resolver seed and the track fold.
- A Tracy A/B of the pass against the commit before, on the same scene and
  camera, two baseline runs for the noise floor. Fog's split cost 9 µs per
  frame on the showcase scene; that number is in the plan and the commit.

## What not to convert

The rule is about appearance. It does not apply to:

- **Simulations.** The motion field, the fog density field, the water FFT,
  physics. A node dial there sets how much simulation runs, not how it looks;
  the look is what a shader does with the result.
- **Techniques whose shape is the point.** Shadows, TAA, Hi-Z culling, DDGI,
  SSR, the bloom chain. An author wants a quality knob, not a hook, and a hook
  inside a multi-pass chain is a contract nobody can keep when the chain
  changes.
- **Anything with a CPU twin.** The water field and `volume_occupancy()` must
  match their C++ half, so their shape cannot be authored without also
  authoring the mirror.

Every type is a permanent contract: template, hook table, docs, lint, tests,
and old shaders that must keep compiling. Add one when a project actually
cannot author something, not ahead of it.

## Where it has been applied

| System | Type | State |
|---|---|---|
| Surfaces | `spatial` | The original. |
| Sky | `sky` | Type and hooks, but values still on the node. |
| Game UI | `ui` | Per-material pipelines. |
| Terrain | `terrain` | `.fjmat` reference, default shader. |
| Water | `water` | `.fjmat` reference, per-node instance for scripts. |
| Fog | `fog` | All eight parts. The worked example; plan in `Plans/done/environment/fog-shaders-2026-09-22.md`. |
| Post-processing | `post` | `.fjmat` on Post Process Volumes, default shader, tracks. The tone curve left the project settings. See [Post-Process.md](../Post-Process.md). |

Next in line, in the order they earn it: the sky's values into a `.fjmat`
with params; the sun shafts' look onto the sun's material.
