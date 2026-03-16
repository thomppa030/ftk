# Creating a Project

## Via the Hub

Launch `fjell-hub`, click "New Project", enter a name and directory path. The hub scaffolds the directory structure and opens the editor.

## Via the Editor

In the editor: Project → New Project. Same dialog.

## Directory Structure

A new project looks like:

```
MyProject/
  project.fjell          Project manifest
  scenes/
    main.fjell           Default scene
  assets/
    models/              glTF/GLB files
    textures/            PNG/JPG files
  scripts/               C++ component source
    CMakeLists.txt       Build config for game.so
```

## project.fjell

```json
{
  "name": "MyProject",
  "version": "0.1.0",
  "window": {
    "width": 1280,
    "height": 720,
    "title": "My Game"
  },
  "default_scene": "scenes/main.fjell"
}
```

## Adding Assets

Drop `.glb`/`.gltf` files into `assets/models/` and `.png`/`.jpg` into `assets/textures/`. Click "Refresh" in the Assets panel (or restart) to pick them up.

## Adding Scripts

1. Create a `.cpp` file in `scripts/` (see [Writing Components](Writing-Components.md))
2. Set up `scripts/CMakeLists.txt` to build `game.so`
3. Build: `cd scripts && cmake -B build && cmake --build build && cp build/game.so ..`
4. The editor detects the new `.so` and loads it automatically
5. In debug builds, recompiling triggers hot-reload

## Building a Game

Project → Build Game packages the project into a standalone distribution:

```
build/
  fjell              Engine binary
  game.fjell         Runtime config
  game.so            Compiled scripts
  scenes/            Scene files
  assets/            Models, textures
  shaders/           Compiled SPIR-V
```

The engine detects `game.fjell` next to the binary and enters game mode — no editor UI, play starts immediately, ESC quits.
