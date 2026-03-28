# Preset System (`.fjp`) — Implementation Plan

## Context

The UI widget system has a fundamental scaling problem: widgets are matched to controllers by tag name (`FJELL_UI_COMPONENT(HealthBar, "health-bar")`). When two `<health-bar>` instances exist in the same UI, both get separate controller instances of the same class with no way to differentiate which data each should display. The tag-based 1:1 mapping doesn't support multiple instances with different data.

Rather than patching this with UI-specific hacks, we're building **Presets** — a general-purpose reusable node subtree system. UI widgets become a specialization of presets. This also enables worldspace UI (health bar floating over enemies) and benefits the whole engine (reusable enemy setups, room templates, etc.).

`GameUiNode` is replaced by preset instances with a `UiWidgetComponent`.

---

## Phase 1: Core Preset System

**Goal:** Save node subtrees as `.fjp` files, instantiate them into scenes, editor integration.

### Step 1: Extract Node Serialization

The node serialization logic in `Scene::save()` (~lines 471-672 of `scene.cpp`) and the matching deserialization in `Scene::load()` (~lines 714-1186) are monolithic. Extract into shared free functions so both Scene and Preset can use them.

**New file: `src/scene/node_serializer.hpp` + `node_serializer.cpp`**
- `nlohmann::json serialize_node(const Node& node)` — serializes any node type + components + scripts to JSON
- `std::unique_ptr<Node> deserialize_node(const nlohmann::json& j, ResourceManager& resources, const Project* project)` — reconstructs a node from JSON
- Move the `dynamic_cast` chains, component serialization, and script entry serialization here

**Modify: `src/scene/scene.cpp`**
- `Scene::save()` becomes a loop calling `serialize_node()` per object
- `Scene::load()` becomes a loop calling `deserialize_node()` per JSON object
- Add `std::vector<const Node*> Scene::collect_subtree(uint32_t root_id) const` — collects a node and all descendants

**Modify: `src/scene/scene.hpp`**
- Declare `collect_subtree()`

**Verify:** Build, run, save/load a scene — behavior identical to before.

### Step 2: Preset Class

**New file: `src/scene/preset.hpp` + `preset.cpp`**

```cpp
namespace fjell {

struct PresetHeader {
    std::string name;
};

class Preset {
public:
    static Result<Preset> load(const std::string& path,
                               ResourceManager& resources,
                               const Project* project = nullptr);
    Result<> save(const std::string& path) const;

    // Instantiate into scene. Returns root node ID.
    // Remaps preset-local IDs to fresh scene IDs.
    uint32_t instantiate(Scene& scene, ResourceManager& resources,
                         const Project* project = nullptr,
                         int32_t parent_id = -1,
                         Transform offset = {}) const;

    // Build from scene selection (copies the subtree)
    static Preset from_subtree(const Scene& scene, uint32_t root_id);
    static Preset from_selection(const Scene& scene,
                                 const std::unordered_set<uint32_t>& ids);

    [[nodiscard]] const PresetHeader& header() const;
    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& nodes() const;

private:
    PresetHeader header_;
    std::vector<std::unique_ptr<Node>> nodes_;
    uint32_t local_next_id_{1};
};

} // namespace fjell
```

`.fjp` JSON format:
```json
{
    "preset": { "name": "Enemy Soldier" },
    "next_id": 4,
    "objects": [
        {"id": 1, "parent_id": -1, "name": "Root", ...},
        {"id": 2, "parent_id": 1, "name": "Mesh", ...},
        {"id": 3, "parent_id": 1, "name": "Collider", ...}
    ]
}
```

`instantiate()` logic:
1. Clone each node
2. Build remap table: `old_local_id → new_scene_id`
3. Fix `parent_id` references via remap
4. Set root's `parent_id` to caller-provided value
5. Apply transform offset to root
6. Set root's `source` to `"preset:path/to/file.fjp"`
7. Call `scene.add_node()` for each, in parent-before-child order

`from_subtree()` logic:
1. `scene.collect_subtree(root_id)`
2. Clone each node
3. Rebase IDs to local space (1, 2, 3...)
4. Remap `parent_id` references
5. Set root's `parent_id` to -1

**Modify: `src/CMakeLists.txt`** — add `preset.cpp`, `node_serializer.cpp`

**Verify:** Unit test — create nodes, build preset, save to disk, load back, instantiate into empty scene, verify hierarchy and components match.

### Step 3: Editor — Create Preset (Drag to Content Browser)

The workflow for creating a preset: drag a node from the hierarchy panel into the content browser. The content browser receives the drop and saves a `.fjp` file.

**Modify: `src/ui/hierarchy_panel.hpp` + `.cpp`**
- Add ImGui DragDropSource on hierarchy nodes
- Payload type: `"HIERARCHY_NODE"`, payload data: the dragged node's `uint32_t id`
- When multi-selection is active, payload contains the primary selected node (the subtree root)

**Modify: `src/ui/content_browser.hpp`**
- Add `preset` to `FileType` enum
- Add `Delegate<void(const std::string&)> on_instantiate_preset`

**Modify: `src/ui/content_browser.cpp`**
- `classify()`: `.fjp` → `FileType::preset`
- `icon_name()`: preset icon (reuse `"node"` or similar)
- Accept DragDropTarget of type `"HIERARCHY_NODE"` in the file grid area
- On drop: broadcast a new delegate `on_create_preset` with the node ID and the current directory path

**Modify: `src/ui/scene_editor_context.hpp` + `.cpp`**
- Bind `on_create_preset(uint32_t node_id, const std::string& directory)`:
  - Build `Preset` from the node via `Preset::from_subtree(scene, node_id)`
  - Save as `<directory>/<node_name>.fjp`
  - If file already exists, append a number or show rename popup
  - Refresh content browser

**Verify:** Drag a node from hierarchy into the content browser's file grid → `.fjp` file appears in that directory. Open the file — correct JSON with full subtree.

### Step 4: Editor — Instantiate from Content Browser

**Modify: `src/ui/content_browser.cpp`**
- `handle_double_click()`: for `.fjp` files, broadcast `on_instantiate_preset`
- Context menu on `.fjp`: "Instantiate in Scene"
- DragDropSource on `.fjp` files with payload type `"CONTENT_PRESET"` + path string

**Modify: `src/ui/hierarchy_panel.cpp`**
- Accept DragDropTarget of type `"CONTENT_PRESET"` on hierarchy nodes (instantiate as child of drop target)
- Accept DragDropTarget in empty space (instantiate at scene root)
- Nodes with `source` starting with `"preset:"` get a distinct icon tint

**Modify: `src/ui/scene_editor_context.cpp`**
- Bind `on_instantiate_preset(const std::string& path)`:
  - Load `.fjp` via `Preset::load()`
  - Call `preset.instantiate()` with current selection as parent (or scene root)
  - Select the new root node
  - Push an `InstantiatePresetCommand` for undo

**New command in `src/core/commands.hpp`:**
- `InstantiatePresetCommand` — captures all created node IDs for undo (extract them all) / redo (restore them all). Follows `DuplicateCommand` pattern.

**Verify:** Double-click a `.fjp` in content browser → appears in scene. Drag `.fjp` onto a node in hierarchy → instantiates as child. Undo removes all instantiated nodes. Preset indicator icon visible on instantiated roots.

---

## Phase 2: UI Widgets as Presets

**Goal:** Rebuild the UI widget system on top of presets. A widget is a preset whose root has a `UiWidgetComponent`. Script binding via `[[fj::bind]] WidgetType*`. `GameUiNode` deprecated.

### Step 5: UiWidgetComponent

Replace `GameUiNode` (a node type) with `UiWidgetComponent` (a component on any node). This follows the engine's composition pattern — the widget capability is added to a node, not a separate node type.

**New file: `src/ui/game_ui/ui_widget_component.hpp`**

```cpp
namespace fjell {

struct UiWidgetComponent {
    std::string fjui_path;    // path to .fjui template
    std::string widget_type;  // registered UiWidget type name (for controller binding)

    enum class RenderMode : uint8_t {
        screen_space,
        world_space
    };
    RenderMode render_mode{RenderMode::screen_space};

    std::vector<std::string> style_classes;  // per-instance CSS class overrides
};

} // namespace fjell
```

**Modify: `src/scene/node_serializer.cpp`**
- Serialize/deserialize `UiWidgetComponent` on nodes (alongside MeshComponent, Rigidbody, etc.)

**Modify: `src/ui/properties_panel.cpp`**
- Add inspector section for `UiWidgetComponent`: fjui_path picker, widget_type dropdown, render_mode selector, style_classes list editor
- Add `UiWidgetComponent` to the "Add Component" menu

**Modify: `src/scene/component_storage.hpp`** (if needed)
- Ensure `UiWidgetComponent` gets a type ID via `component_type_id<UiWidgetComponent>()`

**Verify:** Add a `UiWidgetComponent` to a node in the inspector. Set `fjui_path`. Save scene, reload — component persists. Save node as preset — component included in `.fjp`.

### Step 6: Revised UiSubsystem

Change `UiSubsystem` from finding `GameUiNode`s to finding nodes with `UiWidgetComponent`.

**Modify: `src/ui/game_ui/ui_subsystem.hpp` + `.cpp`**

`on_play_start()`:
1. Iterate scene nodes, find any with `UiWidgetComponent`
2. For each: create `GameUi`, load `.fjui`/`.fjss` from `fjui_path`
3. Apply `style_classes` to the root UiNode
4. Look up registered `UiComponent` by `widget_type` (if non-empty)
5. Create controller instance, call `begin_play()`
6. Wire `[[fj::bind]]` fields and `[[fj::action]]` methods

`tick()`:
- Same as before — tick controllers, sync bind fields to scoped contexts
- For screen-space widgets: update + record as overlay
- (Worldspace deferred to Phase 3)

`on_play_stop()`:
- Destroy all controller instances and GameUi instances

**Verify:** Create a node with `UiWidgetComponent` pointing to a `.fjui`. Press play — UI renders. Stop — UI cleared. Works identically to old `GameUiNode` path.

### Step 7: Script → Widget Binding via `[[fj::bind]]`

The key change: a `ScriptComponent` on a parent node can bind to child widget instances by field name matching child node `id` or name.

**Modify: `src/scripting/fjell/api.h`**
- Add `FJELL_FIELD_WIDGET_PTR` field type for `[[fj::bind]] WidgetType*` fields
- Add `FJELL_FIELD_WIDGET_VEC` field type for `[[fj::bind]] std::vector<WidgetType*>` fields
- Bump `FJELL_API_VERSION`

**Modify: `tools/fjreflect/parser.hpp` + `.cpp`**
- Recognize `[[fj::bind]]` on pointer-to-UiComponent fields
- Store target type name in `FieldInfo::widget_type_name`

**Modify: `tools/fjreflect/generator.cpp`**
- Generate field descriptors with `FJELL_FIELD_WIDGET_PTR` / `FJELL_FIELD_WIDGET_VEC` types
- Include the target widget type name in the descriptor

**Modify: `src/ui/game_ui/ui_subsystem.cpp`**
- After creating all widget controller instances, resolve script bind fields:
  - For each `ScriptComponent` in the scene that has `FJELL_FIELD_WIDGET_PTR` fields:
    - Find child nodes of the script's node that have `UiWidgetComponent`
    - Match by: child node's name == field name (e.g., `hp_bar` field matches child node named `hp_bar`)
    - Set the pointer to the child's `UiComponent` instance
  - For `FJELL_FIELD_WIDGET_VEC` fields: collect all matching children into the vector

**Usage example:**

```cpp
// Script on a scene node
class HudController : public fjell::ScriptComponent {
    FJELL_COMPONENT(HudController)

    [[fj::bind]] StatBar* hp_bar;   // matches child node named "hp_bar"
    [[fj::bind]] StatBar* mp_bar;   // matches child node named "mp_bar"

    void begin_play() override {
        hp_bar->label = "HP";
        hp_bar->value = 100;
        mp_bar->label = "MP";
        mp_bar->value = 50;
    }
};
```

Scene hierarchy:
```
HUD (Node3D + HudController script)
  ├── hp_bar (Node3D + UiWidgetComponent{widget_type="StatBar"})
  └── mp_bar (Node3D + UiWidgetComponent{widget_type="StatBar"})
```

Both `hp_bar` and `mp_bar` are preset instances of the same StatBar preset, but the script addresses them independently.

**Verify:** Create a parent node with a script that has `[[fj::bind]] StatBar* hp_bar`. Add child preset instances named `hp_bar` and `mp_bar`. Press play — script can set different data on each. Both render independently.

### Step 8: Deprecate GameUiNode

**Modify: `src/scene/game_ui_node.hpp`**
- Add deprecation comment

**Modify: `src/scene/node_serializer.cpp`**
- When loading `"node_type": "game_ui"`, convert to `Node3D` + `UiWidgetComponent` with a deprecation log warning

**Modify: `src/ui/hierarchy_panel.cpp`**
- Remove "Game UI" from the add menu (replaced by instantiating UI presets or adding `UiWidgetComponent`)

**Verify:** Old scenes with `GameUiNode` still load and work (auto-converted). New workflow uses presets + `UiWidgetComponent`.

### Step 9: UiWidget as UiComponent (optional rethink)

With the preset model, `UiComponent` no longer needs the `FJELL_UI_COMPONENT(Name, "tag")` tag parameter. The tag-based matching is gone. Instead, the widget type is declared by name and the script binding resolves by child node name.

**Modify: `src/scripting/fjell/ui_registry.hpp`**
- `FJELL_UI_COMPONENT(T)` — drop the tag parameter (or keep it optional for backward compat during transition)
- The `widget_tag` field in `OwnedUiComponentType` becomes the type name itself

**Modify: `src/scripting/script_manager.cpp`**
- `find_ui_type_by_tag()` → `find_ui_type(const std::string& type_name)` — lookup by type name instead of tag

This is a clean-up step. The old tag-matching path can be removed once all widgets migrate to the preset model.

---

## Phase 3: Worldspace UI

**Goal:** Same UI presets render in 3D, positioned by node transform.

### Step 10: World-Space Rendering

**Modify: `src/ui/game_ui/ui_widget_component.hpp`**
- Add world-space settings:
  ```cpp
  float pixels_per_unit{100.0f};
  bool billboard{true};
  glm::vec2 viewport_size{200.0f, 30.0f};  // virtual layout viewport
  ```

**Modify: `src/ui/game_ui/ui_renderer.hpp` + `.cpp`**
- Add world-space render path: takes a `world_matrix` + `view_projection`, transforms UI quad positions from pixel space to world space
- Billboarding: compute camera-facing orientation from node position

**Modify: `src/ui/game_ui/ui_subsystem.cpp`**
- In `tick()`, for world-space widgets:
  - Get owning node's world transform
  - Compute billboard orientation if enabled
  - Call `game_ui->update()` with virtual viewport size
  - Record into main render pass (not UI overlay pass)

**Modify: `src/renderer/vulkan_context.hpp` + `.cpp`**
- Add world-space UI render callback in the main color pass (after opaque, with transparency)

**Modify: `src/ui/properties_panel.cpp`**
- Show world-space settings when `render_mode == world_space`

**Verify:** Take a health bar preset, set render mode to `world_space`, parent to an enemy. Enemy moves → bar follows. Bar is occluded by walls. Billboard faces camera. `pixels_per_unit` controls size.

---

## Phase 4: Future (not designed in detail)

- **Linked instances** — `PresetInstanceComponent` stores source path, changes to `.fjp` propagate to instances
- **Override tracking** — per-instance property overrides shown in inspector (bold = overridden), "Reset to Preset" context menu
- **Nested presets** — presets containing instances of other presets, recursive instantiation

---

## Files Summary

### Phase 1 (new)
- `src/scene/node_serializer.hpp` + `.cpp`
- `src/scene/preset.hpp` + `.cpp`

### Phase 1 (modified)
- `src/scene/scene.hpp` — `collect_subtree()`
- `src/scene/scene.cpp` — refactor to use `node_serializer`, add `collect_subtree()`
- `src/core/commands.hpp` — `InstantiatePresetCommand`
- `src/ui/content_browser.hpp` — `FileType::preset`, `on_instantiate_preset`, `on_create_preset`
- `src/ui/content_browser.cpp` — classify, icon, double-click, drag-drop source/target
- `src/ui/hierarchy_panel.hpp` — drag-drop source for nodes
- `src/ui/hierarchy_panel.cpp` — drag-drop source, preset icon, drag-drop target for `.fjp`
- `src/ui/scene_editor_context.cpp` — bind preset delegates (create + instantiate)
- `src/CMakeLists.txt`

### Phase 2 (new)
- `src/ui/game_ui/ui_widget_component.hpp`

### Phase 2 (modified)
- `src/scene/node_serializer.cpp` — serialize `UiWidgetComponent`
- `src/ui/game_ui/ui_subsystem.hpp` + `.cpp` — find `UiWidgetComponent`, script→widget binding
- `src/ui/properties_panel.cpp` — `UiWidgetComponent` inspector
- `src/scripting/fjell/api.h` — new field types, API version bump
- `src/scripting/fjell/ui_registry.hpp` — drop tag from macro
- `src/scripting/script_manager.cpp` — type lookup by name
- `tools/fjreflect/parser.hpp` + `.cpp` — widget bind fields
- `tools/fjreflect/generator.cpp` — widget bind descriptors
- `src/scene/game_ui_node.hpp` — deprecation
- `src/ui/hierarchy_panel.cpp` — remove "Game UI" from add menu

### Phase 3 (modified)
- `src/ui/game_ui/ui_widget_component.hpp` — world-space settings
- `src/ui/game_ui/ui_renderer.hpp` + `.cpp` — world-space render path
- `src/ui/game_ui/ui_subsystem.cpp` — world-space update
- `src/renderer/vulkan_context.hpp` + `.cpp` — world-space UI callback
- `src/ui/properties_panel.cpp` — world-space settings
