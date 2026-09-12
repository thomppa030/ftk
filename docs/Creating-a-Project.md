# Creating a Project

## Via the Hub

Launch `fjell-hub`, click "New Project", enter a name and directory path. The hub hands
the name to the editor, which scaffolds the directory structure and opens it.

## Via the Editor

In the editor: Project > New Project. Same dialog.

## Directory Structure

A new project looks like:

```
MyProject/
  project.fjell          Project manifest
  .gitignore             Excludes build output and editor state
  scenes/
    main.fjell           Default scene
  assets/
    models/              glTF/GLB files
    textures/            PNG/JPG files
  scripts/               C++ component source
    CMakeLists.txt       Generated — regenerated on every build
```

`scripts/CMakeLists.txt` is written by the engine each time scripts are built, with
paths pointing at whichever engine produced it, so the generated `.gitignore` keeps it
out of version control along with `.fjell/`, build output, and the compiled game module.
The `.gitignore` is only written when absent, so your own additions survive.

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

Drop `.glb`/`.gltf` files into `assets/models/` and `.png`/`.jpg` into `assets/textures/`. Open the Content Browser (Ctrl+Space) to browse and manage your project files. Models can be added to the scene by double-clicking them or right-click > Add to Scene.

## Adding Scripts

The easiest way is from the editor:

1. **Scripts > New Component** — generates a template .cpp file with the boilerplate filled in
2. **Scripts > Build & Reload** — the engine auto-generates `register_all.cpp`, runs cmake, and hot-reloads the .so

You can also do it manually:
1. Create a `.cpp` file in `scripts/` (see [Writing Components](Writing-Components.md))
2. Scripts > Build & Reload (or build manually: `cd scripts && cmake -B build && cmake --build build`)
3. In debug builds, recompiling triggers hot-reload automatically

Scripts show up in the Content Browser. Double-clicking one opens it in whichever editor
your system associates with the file type.

## Building a Game

Project > Build Game packages the project into a standalone distribution:

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
