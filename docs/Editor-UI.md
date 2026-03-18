# Editor UI

The editor is built with Dear ImGui (docking branch) and ImGuizmo. The layout is a dockspace — panels can be rearranged, resized, and tabbed freely.

## Layout

```
+-------------------------------------+-----------------+
| Menu Bar (File, Project, View, Layout, Scripts)        |
+-------------------------------------+-----------------+
|                                     | Hierarchy       |
|           Viewport                  | History (tab)   |
|                                     +-----------------+
+-------------------------------------+ Properties      |
| Stats / Console (tabs)              |                 |
+-------------------------------------+                 |
| Code Editor                         |                 |
+-------------------------------------+-----------------+

         [ Content Browser — overlay, Ctrl+Space ]
```

The Content Browser is not docked — it's a bottom-anchored overlay that slides up with animation when toggled. History is tabbed alongside Hierarchy on the right.

## Panels

### Viewport

Renders the scene as an offscreen image displayed via `ImGui::Image`. You can have multiple viewports open at once (View > Add Viewport).

Each viewport can switch between visualization sources: final color, depth, wireframe, normals, UV, overdraw, meshlet visualization, and light heatmap.

- **Fly camera** — hold right-click and drag to fly around the scene
- **ImGuizmo gizmo** — translate (W), rotate (E), or scale (R) the selected object
- **Selection outline** — selected objects get a visible outline
- **Click to select** — click an object in the viewport to select it

### Hierarchy

Tree view of all scene objects. The History panel shares the same space as a tab.

- Click to select
- Drag-and-drop to reparent (cycle detection prevents invalid hierarchies)
- Drop on empty space to unparent (make root)
- **+** button at the top opens Add Primitive (Cube, Plane, Sphere)
- Right-click empty space for the same add menu
- **X** button to delete an object
- Double-click to focus the camera on that object

### History

Undo/redo history in the style of Godot's history panel. Shows every recorded action with the current position highlighted. Undone entries are greyed out. Click any entry to jump directly to that point. "Initial State" sits at the top of the list.

### Properties

Inspector for the selected object:

- **Name** — editable text field
- **Transform** — position, rotation (degrees), scale drag floats
- **Gizmo mode** — Move (W), Rotate (E), Scale (R)
- **Source** — shows "primitive:cube" or the model path
- **Rigidbody** — body type, mass, friction, restitution, damping, gravity toggle
- **Collider** — shape with shape-specific parameters, offset
- **Particle Emitter** — emitter settings for the VFX system
- **Components** — per-component inspector UI, right-click to remove
- **Add Component** — dropdown of available component types from loaded .so
- **Directional Light** — direction, color, intensity, ambient
- **Point Lights** — position, color, intensity, range
- **Spot Lights** — position, direction, color, intensity, range, cone angles

### Content Browser

Toggled with **Ctrl+Space**. Slides up from the bottom as an overlay — it's not part of the docked layout.

Dual-pane view: a directory tree on the left, a file grid with icons on the right. Breadcrumb navigation sits above the grid.

- Icons distinguish folders, scripts, meshes, textures, and shaders
- Context menus per file type (right-click)
- **F2** to rename a file
- **Delete** to delete (with confirmation dialog)
- **Backspace** to navigate up one directory
- Drag-and-drop files between folders
- Double-click behavior depends on file type: scripts open in the neovim panel, models get added to the scene, scenes get loaded, folders get navigated into

### Stats

GPU name, FPS, frame time (ms), draw calls, triangle count, culled object count, MSAA mode, mesh shader toggle with meshlet count, and a frame time graph.

### Console

Log output from the engine with severity coloring. Auto-scrolls to the latest entry. Has a level filter to show/hide by severity.

### Code Editor

Embedded neovim instance via libvterm. Opens when you double-click a script in the Content Browser. Opening another script switches the buffer directly — no swap file warnings.

## Menu Bar

### File
- **Save Scene** — write current scene to disk (Ctrl+S)
- **Load Scene** — reload from disk

### Project
- **New Project** — create a project directory structure
- **Open Project** — open an existing project by path
- **Close Project** — return to standalone mode
- **Build Game** — package the project for distribution

### View
- **Add Viewport** — open an additional viewport

### Layout
- **Save Layout** — overwrite the active layout
- **Save Layout As...** — save under a new name
- List of saved layouts, with a checkmark on the active one

### Scripts
- **New Component** — create a new script component
- **Build & Reload** — recompile scripts and hot-reload them

## Play Mode

The editor UI is replaced by a full-window viewport with an overlay bar. The cursor is captured for FPS camera control. Press **ESC** or click the Stop button to return to the editor.

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| W | Translate gizmo |
| E | Rotate gizmo |
| R | Scale gizmo |
| Ctrl+S | Save scene |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z / Ctrl+Y | Redo |
| Ctrl+Space | Toggle content browser |
| F2 | Rename (content browser) |
| Delete | Delete with confirmation (content browser) |
| Backspace | Navigate up (content browser) |
| ESC | Deselect (editor) / Stop play (play mode) |
