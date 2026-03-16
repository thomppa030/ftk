# Hub Launcher

`fjell-hub` is a lightweight project launcher — separate binary with minimal Vulkan setup (no scene, no physics, no scripting).

## What It Does

1. Shows the Fjell mountain logo (procedurally rendered from SVG polyline data via ImDrawList)
2. Lists recent projects (persisted in `~/.config/fjell/hub.json`)
3. Provides New Project and Open Project dialogs
4. On project selection: shows splash screen with animated logo and spinner
5. Forks the editor process via `fork()` + `execl()` with the project path as argument

## Architecture

The hub uses the same renderer primitives as the editor (Device, Swapchain, ImGuiLayer) but doesn't create a VulkanContext, Pipeline, Scene, or any of the engine systems. It's essentially a minimal Vulkan + ImGui application.

```
main()
  ├── Window + Device + Swapchain
  ├── ImGuiLayer (dynamic rendering, same as editor)
  ├── HubUI (project browser + splash screen)
  └── render loop
        vkCmdBeginRendering → ImGui → vkCmdEndRendering
```

## Recent Projects

Stored in `~/.config/fjell/hub.json`:

```json
{
  "recent": [
    "/home/user/projects/my-game",
    "/home/user/projects/test-project"
  ]
}
```

Updated when a project is opened. Most recent first.
