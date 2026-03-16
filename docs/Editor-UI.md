# Editor UI

The editor is built with Dear ImGui (docking branch) and ImGuizmo.

## Layout

```
+-------+------------------------+-----------+
| Menu Bar (File, Project)                    |
+-------+------------------------+-----------+
|       |                        | Hierarchy |
| Assets|      Viewport          |           |
|       |   (scene render +      +-----------+
|       |    gizmo overlay)      | Properties|
|       |                        |           |
+-------+------------------------+-----------+
| Stats / Console / Code Editor               |
+---------------------------------------------+
```

The layout is a dockspace — panels can be rearranged, resized, and tabbed freely.

## Panels

### Viewport

Displays the offscreen-rendered scene as an `ImGui::Image`. Contains:

- **Play/Stop button** — toggles play mode
- **Colliders checkbox** — toggles wireframe collider visualization
- **ImGuizmo gizmo** — translate/rotate/scale the selected object (keyboard shortcuts: 1/2/3)

### Hierarchy

Tree view of all scene objects. Features:
- Click to select
- Drag-and-drop to reparent (cycle detection prevents invalid hierarchies)
- Drop on empty space to unparent (make root)
- X button to delete

### Properties

Inspector for the selected object:

- **Name** — editable text field
- **Transform** — position, rotation (degrees), scale drag floats
- **Gizmo** — translate/rotate/scale radio buttons
- **Source** — read-only, shows "primitive:cube" or model path
- **Rigidbody** — add/remove, body type combo, mass, friction, restitution, damping, gravity toggle
- **Collider** — add/remove, shape combo with shape-specific params, offset
- **Components** — auto-generated or custom inspector UI per component, right-click to remove
- **Add Component** — dropdown of available component types from loaded .so
- **Lighting** — direction, color, intensity, ambient

### Assets

- **Primitives** — one-click add cube/plane/sphere
- **Models** — lists `.glb`/`.gltf` from `assets/models/`, click to add to scene
- **Scripts** — lists `.cpp`/`.h` files from `scripts/`, click to open in neovim panel
- **Scenes** — lists `.fjell` files from `scenes/`, click to load

### Stats

Single-line bar: GPU name, FPS, frame time (ms), draw calls, triangle count, frame time graph.

### Console

Log output from the engine with severity coloring. Auto-scrolls. Shows `[script]`-prefixed messages from game code.

### Code Editor

Embedded neovim instance via libvterm. Opens when clicking a script file in the Assets panel. Full terminal emulation with TrueColor support.

## Menu Bar

### File
- **Save Scene** — write current scene to disk
- **Load Scene** — reload current scene from disk

### Project
- **New Project** — create project directory structure
- **Open Project** — open existing project by path
- **Close Project** — return to standalone mode
- **Build Game** — package project for distribution

## Play Mode

When playing, the editor UI is replaced by a full-window viewport with an overlay bar showing "Press ESC to stop" and a Stop button. The cursor is captured for FPS camera control.

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| 1 | Translate gizmo |
| 2 | Rotate gizmo |
| 3 | Scale gizmo |
| ESC | Deselect (editor) / Stop play (play mode) |
