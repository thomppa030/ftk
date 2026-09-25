# File Formats

> Stub — fill in.

## Scope
Every `.fj*` file at a glance. Each entry: one-line description, text vs binary, schema link, who reads it, who writes it.

## Planned entries

| Ext | Kind | Purpose | Details |
|---|---|---|---|
| `.fjscene` | text (JSON) | Scene graph | [Scene System](../Architecture/Scene-System.md) |
| `.fjmesh` | binary | Mesh + meshlets + LODs | [Asset Pipeline](../Architecture/Asset-Pipeline.md) |
| `.fjskel` | binary | Skeleton (joint hierarchy + bind pose) | [Skeletons & Clips](../Animation/Skeletons-and-Clips.md) |
| `.fjanim` | binary | Animation clip | [Skeletons & Clips](../Animation/Skeletons-and-Clips.md) |
| `.fjsl` | text | Spatial shader | [FJSL](../Materials/FJSL.md) |
| `.fjmat` | text (JSON) | Material asset | [FJMAT](../Materials/FJMAT.md) |
| `.fjui` | text | UI markup | [FJUI](../Game-UI/FJUI.md) |
| `.fjss` | text | UI stylesheet | [FJSS](../Game-UI/FJSS.md) |
| `.fjwidget` | text | UI widget (MVC) | [Widgets](../Game-UI/Widgets.md) |
| `.fjvfx` | text | VFX particle system | [Particle System](../VFX/Particle-System.md) |
| `.fjaction` | text | Input action definition | [Input Actions](../Scripting/Input-Actions.md) |
| `.fjinput` | text | Input mapping context | [Input Actions](../Scripting/Input-Actions.md) |
| `.fjgraph` | text | Framegraph layout | [Framegraph](../Architecture/Framegraph.md) |
| `.fjp` | text | Preset | TBD |
| `.fjday` | text (JSON) | Day profile — the look of a day over 24 hours | [Day Profiles](../Environment/Day-Profiles.md) |
| `.fjweather` | text (JSON) | Weather event — phases, tracks, precipitation, lightning | [Weather](../Environment/Weather.md) |
| `.fjclimate` | text (JSON) | Climate — which weather follows which, and how likely | [Weather](../Environment/Weather.md#climates-fjclimate) |
| `.fjlut` | text (JSON) | Colour grading table: `{"name", "source"}`, the source a strip PNG or a `.cube` beside it | [Post-Processing](../Post-Process.md#colour-grading-with-a-lut) |

Each format will get a short schema section here once filled in.
