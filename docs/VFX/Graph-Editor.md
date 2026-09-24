# VFX Graph Editor

> Stub — fill in.

## Scope
Node-graph authoring UI drawn on the editor kit's canvas (`src/ui/vfx_graph.*`). Nodes are modules + expressions; the graph is serialized into the `.fjvfx` asset and reduced to a flat module list at runtime.

## Planned sections
- Opening the editor (context tab, double-click `.fjvfx`)
- Module nodes — emit / init / update / render
- Expression nodes — math, time, random, curve, attribute read
- CPU evaluation — expressions baked at compile, or evaluated per-frame on CPU for cheap params
- GPU evaluation — expressions compiled into the compute shader for per-particle params
- Connecting modules — order, port types, validation errors
- Serialization — node positions, params, links
- Live preview in the viewport
- Hot-reload behavior
- Patterns / recipes (sparks, smoke plume, magic beam)
