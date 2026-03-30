# Content Browser: Create New Submenu + Utilities

## Context

The content browser's right-click context menu has no way to create new engine asset files. Users must create them manually on disk. The menu also lacks basic utilities like "Show in File Manager" and "Copy Path". Additionally, every `case` branch in `draw_context_menu()` duplicates the same Rename/Delete code — we can clean this up while adding the new features.

## Plan

### Step 1: Create `src/ui/asset_templates.hpp`

Header-only file with a `constexpr` array of `AssetTemplate` structs. Each entry has: `label` (menu display name), `filename` (default name without ext), `extension`, `content` (template string). Separator entries use empty `label`.

10 asset types grouped with separators: Input (.fjaction, .fjinput) | Materials (.fjmat, .fjsl) | VFX (.fjvfx) | UI (.fjui, .fjss, .fjwidget) | Scene (.fjell, .fjp).

Templates match what each parser expects (e.g. `.fjvfx` includes `"version": 2`, `.fjp` includes `preset.name` + `next_id` + `objects`).

### Step 2: Add 3 private methods to `content_browser.hpp`

- `create_asset_file(string_view filename, string_view extension, string_view content)` — write file, refresh, enter rename mode on it
- `draw_create_new_submenu()` — iterates `kAssetTemplates`, draws ImGui submenu
- `draw_common_context_items(int index)` — draws Create New submenu + Rename + Delete + Show in File Manager + Copy Path

No new state members needed — reuses existing `renaming_`, `rename_index_`, `rename_buf_`.

### Step 3: Implement the 3 methods in `content_browser.cpp`

**`create_asset_file`**: Writes template to `current_dir_/filename.ext`. If file exists, appends `_1`, `_2`, etc. After `refresh_current_dir()`, finds the new entry by name and sets `renaming_ = true` + `rename_index_` + `rename_buf_` to enter inline rename mode.

**`draw_create_new_submenu`**: `ImGui::BeginMenu("Create New")` → loop `kAssetTemplates` → separator for empty labels, `MenuItem` otherwise → calls `create_asset_file`.

**`draw_common_context_items`**: Calls `draw_create_new_submenu()`, then Rename, Delete, separator, Show in File Manager (`platform::spawn_detached({"xdg-open", dir})`), Copy Path (`ImGui::SetClipboardText` with relative path).

### Step 4: Refactor `draw_context_menu()` per-file branches

Replace the duplicated Rename/Delete blocks in every `case` with a single `draw_common_context_items(context_menu_index_)` call. Each case keeps only its type-specific items (Open, Add to Scene, Preview, etc.), then calls the common helper.

### Step 5: Kill modal popups for New Folder and New Script

Remove the `show_new_folder_popup_`, `new_folder_buf_`, `show_new_script_popup_`, `new_script_buf_` state members and their modal popup code from both `.hpp` and `.cpp`. Replace with the same inline pattern: create with default name, refresh, enter rename mode.

- "New Folder" → `create_directory("new_folder")`, refresh, rename mode
- "New Script" → `create_asset_file("new_script", ".cpp", "#include <iostream>\n")`, refresh, rename mode

Add a `create_folder()` helper method that follows the same pattern as `create_asset_file` (deduplicate name, refresh, enter rename mode).

### Step 6: Update empty-space branch

```
Create New ►  (submenu)
─────────────
New Folder
New Script
─────────────
Refresh
─────────────
Show in File Manager
Copy Path
```

New Folder and New Script now directly create + rename inline, no modal.

## Files

| File | Action |
|------|--------|
| `src/ui/asset_templates.hpp` | Create |
| `src/ui/content_browser.hpp` | Add method declarations, remove popup state members |
| `src/ui/content_browser.cpp` | Add include, implement methods, refactor context menu, remove modal popup code |

## Verification

- Build with `cmake --build build -j$(nproc)`
- Run engine, open content browser, right-click empty space → "Create New" submenu appears with all 10 types
- Click any type → file created, entry appears in grid in rename mode
- Right-click a file → same "Create New" submenu + "Show in File Manager" + "Copy Path"
- Create duplicate → appends `_1` suffix instead of overwriting
- "Copy Path" → clipboard has relative path
- "Show in File Manager" → file manager opens to current directory
