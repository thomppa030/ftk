# Fjell Engine

Vulkan game engine written in C++23, targeting Linux and Windows.

## Documentation

### Getting Started
- [Building & Running](Building-and-Running.md)
- [Project Structure](Project-Structure.md)
- [Creating a Project](Creating-a-Project.md)

### Editor
- [Editor UI](Editor-UI.md)
- [Hub Launcher](Hub-Launcher.md)

### Architecture
- [Engine Overview](Engine-Overview.md)
- [Main Loop](Main-Loop.md)
- [Renderer](Renderer.md)
- [Materials](Materials.md)
- [Render Passes](Render-Passes.md)
- [Scene System](Scene-System.md)
- [Physics](Physics.md)

### Environment
- [Time of Day](Environment/Time-of-Day.md) — the clock, the sun's path, the moon and the stars
- [Day Profiles](Environment/Day-Profiles.md) — `.fjday`, the look of a day keyed over 24 hours
- [Weather](Environment/Weather.md) — `.fjweather` events, `.fjclimate`, wetness, wind, lightning and shelter
- [Fog Volumes](Environment/Fog-Volumes.md) — placed banks of fog, with drifting noise for creeping mist
- [Sky Shaders](Environment/Sky-Shaders.md) — `shader_type sky`, its built-ins, and how the sky lights the world

### Scripting
- [Scripting Overview](Scripting-Overview.md)
- [Engine API Reference](Engine-API-Reference.md)
- [Writing Components](Writing-Components.md)
