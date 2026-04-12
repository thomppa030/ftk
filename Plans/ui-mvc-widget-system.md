# UI Widget System Redesign: MVC with Typed Props

## Context

The game UI widget system works but has a painful workflow:
- `UiContext` is a `map<string, string>` — typed C++ fields get serialized to strings every frame, then parsed back for rendering. No type safety, typos are silent.
- Actions are disconnected — markup `on:click="foo"` and C++ `bind_action("foo", ...)` are validated against nothing.
- `.fjwidget` is a JSON file that just points at separate `.fjui` + `.fjss` files — pure boilerplate.
- Widget prop passing between parent and child is all string-to-string through `prop_map`.

The goal is a proper Model-View-Controller workflow: `.fjwidget` becomes a self-describing widget file with typed prop declarations, inline markup, and stylesheet reference. Actions are validated at load time. The string map stays as an escape hatch for widgets without a C++ controller.

## Target .fjwidget Format

```html
<widget class="HealthBar" style="health_bar.fjss">
    <prop name="hp" type="float" default="100"/>
    <prop name="max_hp" type="float" default="100"/>

    <div class="health-bar">
        <text>HP: {hp} / {max_hp}</text>
        <bar value="{hp}" max="{max_hp}"/>
    </div>
</widget>
```

- `class` attribute: optional C++ controller class name
- `style` attribute: relative path to `.fjss` stylesheet
- `<prop>` children: typed contract (name, type, default)
- Remaining children: the markup body (same syntax as `.fjui`)
- View-only widgets (no `class` attribute) work with just props and string API

---

## Phase 1: WidgetAsset data model + .fjwidget markup parser

**Goal**: Replace the JSON `.fjwidget` format with the new `<widget>` markup format. Parse `<prop>` declarations, store parsed markup tree.

### Files to modify

**`src/ui/game_ui/widget_asset.hpp`**
- Add `PropDecl` struct: `{ string name, string type, string default_value }`
- Add `ActionDecl` struct (for Phase 4): `{ string name, string param_type }`
- Extend `WidgetAsset` with:
  - `vector<PropDecl> props`
  - `vector<ActionDecl> actions` (populated later)
  - `unique_ptr<UiNode> markup_root` (parsed markup body)
- Remove `fjui_path` field (markup is inline now)

**`src/ui/game_ui/widget_asset.cpp`**
- Rewrite `load_widget_asset()`: call `FjuiParser::parse()` on the file, verify root tag is `<widget>`, extract `class`/`style` attributes, walk children to separate `<prop>` tags from markup body, store markup body children as a wrapper UiNode in `markup_root`
- Remove old JSON parsing path

### Verification
- Write test `.fjwidget` in new format. Call `load_widget_asset()`. Check `props`, `class_name`, `fjss_path`, `markup_root` are correct.

---

## Phase 2: Plumb WidgetAsset through GameUi + UiSubsystem

**Goal**: New-format `.fjwidget` files render using their inline markup tree instead of loading a separate `.fjui` file.

### Files to modify

**`src/ui/game_ui/game_ui.hpp` + `game_ui.cpp`**
- Add overload: `void load_from_tree(unique_ptr<UiNode> root, const string& fjss_path, const string& base_dir)`
- Does same post-processing as `load()` (widget resolution, text template init, texture resolution, watcher setup) but skips file I/O and parsing

**`src/ui/game_ui/ui_data_bind.hpp`**
- Make `clone_node()` public static (currently private). Needed by UiSubsystem to deep-copy `markup_root` per widget instance.

**`src/ui/game_ui/ui_subsystem.cpp`**
- In `on_play_start()`: deep-clone `markup_root` from the loaded `WidgetAsset` and call `load_from_tree()`. Remove the old `load(fjui_path, fjss_path)` path.

### Verification
- Create `.fjwidget` with inline markup + `UiWidgetComponent`. Press play. Widget renders correctly.

---

## Phase 3: Typed prop storage in UiContext

**Goal**: Float/int/bool values stored natively. Eliminates the `float -> to_string -> stof` roundtrip on every frame.

### Files to modify

**`src/ui/game_ui/ui_context.hpp`**
- Add typed storage: `unordered_map<string, float> typed_floats_`, same for int/bool
- `set_float()`: write to `typed_floats_` AND format to `values_` string map (text templates like `"HP: {hp}"` still need the string). Use a compact format (not `std::to_string` which gives `72.000000` — use `std::format` or `snprintf` for `"72"`)
- `set_int()`, `set_bool()`: same dual-write pattern
- Add typed getters: `optional<float> get_float(key)`, etc. — check typed map first, walk parent chain
- `clear()`: also clear typed maps

**`src/ui/game_ui/ui_data_bind.cpp`**
- Bar handling (lines 90-107): try `ctx.get_float(key)` first for simple `{key}` references. Fall back to `resolve_template() + stof()` only when the attribute contains mixed text.
- Prop sync (lines 29-33): when `widget_props` are available on the node, use typed setters

**`src/ui/game_ui/ui_node.hpp`**
- Add `vector<PropDecl> widget_props` to `UiNode` (populated during widget resolution from WidgetAsset::props)

### Verification
- Existing bar widgets render identically (visual regression check)
- Set a float via C++ controller, verify `get_float()` returns it without string conversion

---

## Phase 4: Action metadata in runtime type system

**Goal**: `fjreflect` emits action descriptors. `OwnedUiWidgetType` carries action info at runtime. Prerequisite for action validation.

### Files to modify

**`src/scripting/fjell/api.h`**
- Add `FjellUiActionDesc` struct: `{ const char* name, uint8_t param_type }` where param_type: 0=none, 1=int, 2=float, 3=vec2
- Bump `FJELL_API_VERSION` from 9 to 10

**`src/scripting/fjell/ui_registry.hpp`**
- Add to `FjellUiWidgetTypeOOP`: `const FjellUiActionDesc* actions; uint32_t action_count;`
- Add to `FjellUiGeneratedInfo`: same two fields
- In `register_ui_widget<T>()`: copy `gen->actions` and `gen->action_count` into desc

**`src/scripting/script_manager.hpp`**
- Add `OwnedUiActionDesc`: `{ string name, uint8_t param_type }`
- Add `vector<OwnedUiActionDesc> actions` to `OwnedUiWidgetType`

**`src/scripting/script_manager.cpp`**
- In `fjell_reg_ui_component()`: copy action descriptors from `desc->actions` into `owned.actions`

**`tools/fjreflect/generator.cpp`**
- After action trampolines, emit action descriptor table:
  ```cpp
  extern "C" const FjellUiActionDesc fjreflect_ClassName_ui_actions[] = { ... };
  extern "C" const uint32_t fjreflect_ClassName_ui_action_count = N;
  ```
- Include these in `FjellUiGeneratedInfo` initialization

### Verification
- Compile a widget with `[[fj::action]] void heal(float amount)`. In debugger/log, verify `OwnedUiWidgetType::actions` contains `{name: "heal", param_type: 2}`.

---

## Phase 5: Prop validation at load time

**Goal**: `{key}` references in markup validated against declared `<prop>` names. Unknown keys produce warnings.

### Files to modify

**`src/ui/game_ui/widget_asset.cpp`**
- After parsing new-format `.fjwidget`, walk markup tree collecting all `{key}` references from text templates and attributes
- Check each key against `props` vector. Unmatched keys (excluding reserved like `__index`) → warning

**`src/ui/game_ui/widget_resolver.hpp` + `widget_resolver.cpp`**
- Add `unordered_map<string, WidgetAsset> widget_cache_`
- Change `resolve_node()` to resolve `widgets/<tag>.fjwidget` (drop `.fjui` fallback)
- Validate parent-provided attributes against declared props (unknown attr → warning, required prop without default and not provided → warning)
- Copy `WidgetAsset::props` into the `UiNode::widget_props` field
- Set prop defaults in scoped context using typed setters

### Verification
- Widget with `<prop name="hp">` but markup uses `{health}` → warning
- Parent uses `<health-bar foo="bar">` where `foo` is not a declared prop → warning

---

## Phase 6: Action validation at load time

**Goal**: `on:click` targets validated against the widget's C++ class actions (when a class exists). Mismatch → warning.

### Files to modify

**`src/ui/game_ui/ui_subsystem.cpp`**
- After `collect_widgets()` and before `begin_play()`: for each widget with a C++ type, walk its markup subtree, extract `on:click` targets, check against `OwnedUiWidgetType::actions`
- New helper: `validate_widget_actions(const UiNode& node, const OwnedUiWidgetType* type)`
- Unmatched action → warning: `"Widget 'HealthBar': on:click target 'foo' not found in declared actions"`
- For widgets without a C++ class: skip validation (actions come from external `bind_action` calls)

### Verification
- `on:click="nonexistent"` on a widget with C++ class → warning at play start
- `on:click="heal(50)"` where `heal` is declared → no warning

---

## Phase 7: Editor updates

**Goal**: Editor opens new-format `.fjwidget` directly with type-aware mock data panel.

### Files to modify

**`src/ui/ui_editor_context.hpp` + `ui_editor_context.cpp`**
- Store `WidgetAsset loaded_widget_` and `vector<PropDecl> active_props_`
- In `open_ui()`: if new-format `.fjwidget`, use `load_from_tree()` on preview GameUi. Store props in `active_props_`.
- In `draw_mock_data()`: if `active_props_` populated, render type-aware controls:
  - `float` → `ImGui::DragFloat` (with range hints if available)
  - `int` → `ImGui::DragInt`
  - `bool` → `ImGui::Checkbox`
  - `string` → text input (existing)
- Fall back to text inputs for undeclared keys (escape hatch)

**`src/ui/asset_templates.hpp`**
- Update widget template from JSON to new markup:
  ```
  <widget class="" style="">
      <panel id="root">
      </panel>
  </widget>
  ```

### Verification
- Open new-format `.fjwidget` in editor. Mock data shows typed controls from `<prop>` declarations.
- Float slider updates preview in real-time.

---

## Phase 8: Migration cleanup

- Convert existing `test.fjui` to `test.fjwidget` in new format
- Remove old JSON parsing from `widget_asset.cpp` (done in Phase 1)
- Update asset template in `asset_templates.hpp` (done in Phase 7)
- Remove `.fjui` template from asset templates (widgets use `.fjwidget` now, standalone `.fjui` files are gone)

---

## Dependency Graph

```
Phase 1 (parser + data model)
  ├── Phase 2 (plumb through subsystem)
  │     ├── Phase 5 (prop validation) ← also needs Phase 1
  │     └── Phase 7 (editor) ← needs Phases 1-5
  ├── Phase 3 (typed UiContext) ← independent
  └── Phase 4 (action metadata) ← independent
        └── Phase 6 (action validation) ← needs Phase 2
```

Phases 3 and 4 can be done in parallel. Phases 5 and 6 can be done in parallel once their deps are met.

---

## Key files (full paths)

- `src/ui/game_ui/widget_asset.hpp` / `.cpp` — new format parser, PropDecl/ActionDecl
- `src/ui/game_ui/ui_context.hpp` — typed storage alongside string map
- `src/ui/game_ui/ui_data_bind.hpp` / `.cpp` — typed prop resolution, clone_node public
- `src/ui/game_ui/ui_node.hpp` — `widget_props` field
- `src/ui/game_ui/widget_resolver.hpp` / `.cpp` — .fjwidget resolution, prop validation, defaults
- `src/ui/game_ui/game_ui.hpp` / `.cpp` — `load_from_tree()` overload
- `src/ui/game_ui/ui_subsystem.cpp` — plumbing + action validation
- `src/scripting/fjell/api.h` — FjellUiActionDesc, API version bump
- `src/scripting/fjell/ui_registry.hpp` — action fields in type descriptors
- `src/scripting/script_manager.hpp` / `.cpp` — OwnedUiActionDesc storage
- `tools/fjreflect/generator.cpp` — action descriptor table codegen
- `src/ui/ui_editor_context.hpp` / `.cpp` — typed mock data panel
- `src/ui/asset_templates.hpp` — new widget template
