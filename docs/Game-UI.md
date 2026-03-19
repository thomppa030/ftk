# Game UI

The game UI system renders in-game HUD, menus, and overlays — separate from the ImGui editor chrome. You author UI using markup (`.fjui`) and stylesheets (`.fjss`). Layout is computed via Facebook's Yoga flexbox engine. The result is rendered as Vulkan quads on top of the 3D scene.

## Quick Start

Create two files in `assets/ui/`:

**`assets/ui/test.fjui`**
```html
<panel id="hud">
    <text class="label">HP: {player.health} / {player.max_health}</text>
    <div class="health-bar">
        <bar class="health-fill" value="{player.health}" max="{player.max_health}"/>
    </div>
    <div class="slots">
        <div class="slot" on:click="select_1" class:active="slot==1"/>
        <div class="slot" on:click="select_2" class:active="slot==2"/>
    </div>
</panel>
```

**`assets/ui/test.fjss`**
```css
#hud {
    flex-direction: column;
    padding: 16;
    color: white;
    font-size: 14;
}

.health-bar {
    width: 200;
    height: 20;
    background-color: #333333;
    border-radius: 4;
}

.health-fill {
    width: 200;
    height: 20;
    background: linear-gradient(90deg, #22aa22, #66dd44);
    border-radius: 4;
    color: #44cc44;
}

.slot {
    width: 48;
    height: 48;
    background-color: #222222;
    border: 2 solid #555;
    border-radius: 6;
    transition: all 0.15s ease-in-out;
}

.slot:hover {
    border-color: #888;
}

.active {
    border-color: #d4a054;
}
```

The engine loads `assets/ui/test.fjui` + `assets/ui/test.fjss` automatically at startup. Set up the game context to drive it:

```cpp
auto& ctx = vulkan_context_.game_ui().context();
ctx.set("player.health", "72");
ctx.set("player.max_health", "100");
ctx.set("slot", "1");
ctx.bind_action("select_1", [&ctx] { ctx.set("slot", "1"); });
ctx.bind_action("select_2", [&ctx] { ctx.set("slot", "2"); });
```

## .fjui Markup

HTML-like syntax. Tags, attributes, self-closing elements, and comments.

```html
<!-- A comment -->
<tag id="unique" class="one two" custom-attr="value">
    <child/>
    Some text content
</tag>
```

- `id` — unique identifier, matched by `#id` in stylesheets
- `class` — space-separated class list, matched by `.class` in stylesheets
- Other attributes are stored on the node and used by the binding/input systems
- Tag names are freeform — `div`, `panel`, `slot`, whatever makes sense for your UI

### Special Elements

| Element | Description |
|---------|-------------|
| `<text>` | Renders MSDF text. Supports `{key}` data binding in content. |
| `<bar>` | Progress/health bar. Uses `value` and `max` attributes (support `{key}` binding). Renders the element's background/gradient clipped to the fill percentage. |

### Directives

| Attribute | Description | Example |
|-----------|-------------|---------|
| `if="key"` | Show/hide node based on context value truthiness | `<div if="show_debug">` |
| `on:click="action"` | Dispatch named action on click | `<div on:click="open_menu"/>` |
| `class:name="condition"` | Add/remove CSS class dynamically | `<div class:active="slot==1"/>` |

#### class: binding

The condition can be:
- `"key"` — truthy check (non-empty, not `"0"`, not `"false"`)
- `"key==value"` — equality check against a literal

```html
<div class:active="selected_tab==inventory"/>
<div class:visible="show_hud"/>
```

#### {key} Data Binding

Text content and `<bar>` value/max attributes support `{key}` placeholders that resolve from `UiContext`:

```html
<text>HP: {player.health} / {player.max_health}</text>
<bar value="{player.health}" max="{player.max_health}"/>
```

### Node Types

| Type | Description |
|------|-------------|
| Element | Any named tag. Participates in flexbox layout. |
| Text | Raw text between tags. Rendered with MSDF text pipeline. |
| Root | Implicit wrapper if the document has multiple top-level elements. |

## .fjss Stylesheets

CSS-like syntax with selectors and property declarations.

```css
/* Comments */
tag { property: value; }
.class { property: value; }
#id { property: value; }

/* Descendant combinator */
panel .slot { property: value; }

/* Pseudo-class selectors */
.slot:hover { border-color: #888; }
.slot:active { background-color: #444; }
#input:focus { border-color: #4488ff; }
```

### Selectors

| Selector | Specificity | Example |
|----------|-------------|---------|
| `tag` | 1 | `div { ... }` |
| `.class` | 10 | `.panel { ... }` |
| `:pseudo` | 10 | `.slot:hover { ... }` |
| `#id` | 100 | `#hud { ... }` |
| `ancestor descendant` | sum | `panel .slot { ... }` |

Higher specificity wins. Equal specificity: last rule wins.

Pseudo-classes: `:hover` (mouse over), `:active` (mouse down), `:focus` (clicked to focus). Interaction state is tracked per-node and triggers targeted style re-resolution for performance.

### Layout Properties

| Property | Values | Default |
|----------|--------|---------|
| `display` | `flex`, `none` | `flex` |
| `flex-direction` | `row`, `column`, `row-reverse`, `column-reverse` | `column` |
| `justify-content` | `flex-start`, `flex-end`, `center`, `space-between`, `space-around`, `space-evenly` | `flex-start` |
| `align-items` | `flex-start`, `flex-end`, `center`, `stretch`, `baseline` | `stretch` |
| `align-self` | `auto`, `flex-start`, `flex-end`, `center`, `stretch` | `auto` |
| `flex-wrap` | `nowrap`, `wrap`, `wrap-reverse` | `nowrap` |
| `flex-grow` | number | `0` |
| `flex-shrink` | number | `1` |
| `position` | `relative`, `absolute` | `relative` |
| `overflow` | `visible`, `hidden`, `scroll` | `visible` |

### Dimension Properties

All values are in pixels. Use `-1` or `auto` for auto-sizing.

| Property | Description |
|----------|-------------|
| `width`, `height` | Explicit size |
| `min-width`, `min-height` | Minimum size |
| `max-width`, `max-height` | Maximum size |

### Spacing Properties

All accept 1, 2, 3, or 4 values (CSS shorthand):
- `padding: 8` — all sides
- `padding: 8 16` — vertical horizontal
- `padding: 8 16 12` — top horizontal bottom
- `padding: 8 16 12 4` — top right bottom left

| Property | Description |
|----------|-------------|
| `margin` | Outer spacing (shorthand) |
| `margin-top`, `margin-right`, `margin-bottom`, `margin-left` | Individual margins |
| `padding` | Inner spacing (shorthand) |
| `padding-top`, `padding-right`, `padding-bottom`, `padding-left` | Individual padding |
| `gap`, `row-gap`, `column-gap` | Space between flex children |
| `top`, `right`, `bottom`, `left` | Insets for `position: absolute` |

### Visual Properties

| Property | Values | Description |
|----------|--------|-------------|
| `background-color` | color | Solid fill |
| `background` | `linear-gradient(angle, from, to)` | Linear gradient fill |
| `border` | `width solid color` | Border shorthand |
| `border-color` | color | Border stroke color |
| `border-width` | pixels | Border thickness |
| `border-radius` | 1–4 values (px) | Corner rounding (tl, tr, br, bl) |
| `opacity` | 0.0–1.0 | Transparency (inherited, compounds with parent) |
| `box-shadow` | `x y blur [spread] color` | Drop shadow |
| `transition` | `property duration [easing]` | Animate property changes |

### Text Properties

| Property | Values | Description |
|----------|--------|-------------|
| `color` | color | Text color (inherited) |
| `font-size` | pixels | Font size (inherited) |
| `line-height` | pixels | Line spacing (0 = font default) |

### Colors

```css
background-color: #ff6600;           /* 6-digit hex */
background-color: #ff660080;         /* 8-digit hex with alpha */
background-color: #f60;              /* 3-digit shorthand */
background-color: rgba(255, 100, 0, 0.8);  /* rgba (0-255 rgb, 0-1 alpha) */
background-color: rgb(255, 100, 0);  /* rgb */
background-color: transparent;
background-color: white;
```

Named colors: `transparent`, `white`, `black`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `gray`/`grey`, `orange`.

### Gradients

```css
background: linear-gradient(90deg, #22aa22, #66dd44);
background: linear-gradient(to bottom, #444, #222);
```

Directions: angle in degrees, or `to top`, `to bottom`, `to left`, `to right`.

### Box Shadows

```css
box-shadow: 0 4 8 0 rgba(0, 0, 0, 0.3);   /* x y blur spread color */
box-shadow: 2 2 6 #00000080;               /* spread optional */
box-shadow: none;                           /* remove */
```

### Transitions

Animate style changes smoothly (e.g. on hover state change):

```css
transition: all 0.15s ease-in-out;
transition: opacity 0.3s linear;
transition: border-color 0.2s ease-out;
```

Format: `property duration [easing]`

Easing functions: `linear`, `ease-in`, `ease-out`, `ease-in-out`

Animatable properties: `opacity`, `border-width`, `background-color`, `border-color`, `color`.

### Inherited Properties

These properties cascade from parent to child unless overridden:
- `color`
- `font-size`
- `line-height`
- `opacity` (compounds — child opacity is relative to parent)

## UiContext

The bridge between game code and the UI. Game logic sets values; the UI reads them via `{key}` bindings.

```cpp
auto& ctx = vulkan_context_.game_ui().context();

// Set values (read by {key} bindings, if directives, class: bindings)
ctx.set("player.health", "72");
ctx.set("player.max_health", "100");
ctx.set_int("score", 1500);
ctx.set_float("stamina", 0.85f);
ctx.set("show_debug", "1");          // truthy for if="show_debug"
ctx.set("active_slot", "2");         // drives class:active="active_slot==2"

// Bind actions (dispatched by on:click)
ctx.bind_action("open_inventory", [&]() { open_inventory(); });
ctx.bind_action("select_slot", [&ctx]() { ctx.set("active_slot", "3"); });

// Read values back
std::string hp = ctx.get("player.health");
```

## How It Works

```
.fjui file  →  FjuiParser     →  UiNode tree
.fjss file  →  FjssParser     →  Stylesheet (rules + selectors)

Per frame:
  UiDataBind      →  resolve {key} templates, if directives, class: bindings
  StyleResolver   →  match selectors (incl. :pseudo), apply properties → UiComputedStyle
  UiInput         →  hit test mouse, update hover/active/focus, dispatch on:click
  StyleResolver   →  re-resolve dirty nodes (interaction state changed)
  UiAnimation     →  interpolate transitioning properties
  UiLayout        →  build Yoga tree, run flexbox, read back pixel rects
  UiRenderer      →  flatten tree to GpuUiQuad + GpuUiGlyph arrays, upload to SSBO
  Vulkan draw     →  quad pipeline (SDF rounded rect) + text pipeline (MSDF)
```

### Rendering Details

The UI renderer follows the same pattern as the sprite renderer:
- SSBO-based instanced quads (no vertex buffers)
- Double-buffered for CPU/GPU overlap
- 128 bytes per quad, 96 bytes per text glyph
- Vertex shader generates quad corners from `gl_VertexIndex`
- Fragment shader uses SDF for anti-aliased rounded rectangles, borders, gradients, and box shadows
- Text fragment shader samples MSDF atlas with median-of-three + screenPxRange
- Alpha blending, no depth test (UI always on top)
- Renders after tone mapping, before ImGui

### Text Rendering

MSDF (Multi-channel Signed Distance Field) text via FreeType + msdfgen:
- Font atlas generated at startup from TTF file (ASCII range, 48px cells, px_range=6)
- Uniform scale across all glyphs for consistent stroke weight
- Kerning table for all codepoint pairs
- Text shaper handles word-wrap and line breaking
- Crisp at any size — no re-rasterization needed

### Frame Graph Integration

The `game_ui` pass runs after `tonemap` and before the debug visualization passes. It renders into the tone-mapped output with `VK_ATTACHMENT_LOAD_OP_LOAD` so the 3D scene is preserved underneath.

### Hot Reload

In debug builds, an inotify watcher monitors the loaded `.fjui` and `.fjss` files. Edit either file while the engine is running and the UI rebuilds automatically — no restart needed.

## File Locations

| Path | Description |
|------|-------------|
| `src/ui/game_ui/ui_types.hpp` | Enums, UiColor, UiRect, UiEdges, UiCorners, UiBoxShadow, UiGradient |
| `src/ui/game_ui/ui_style.hpp` | UiComputedStyle — all layout + visual properties |
| `src/ui/game_ui/ui_node.hpp` | UiNode — DOM tree node with interaction state |
| `src/ui/game_ui/fjui_parser.hpp/.cpp` | .fjui markup parser |
| `src/ui/game_ui/fjss_parser.hpp/.cpp` | .fjss stylesheet parser (with pseudo-class selectors) |
| `src/ui/game_ui/style_resolver.hpp/.cpp` | Selector matching, specificity, cascade, targeted re-resolve |
| `src/ui/game_ui/ui_layout.hpp/.cpp` | Yoga flexbox layout bridge with scroll offset |
| `src/ui/game_ui/ui_input.hpp/.cpp` | Mouse hit testing, interaction state, action dispatch |
| `src/ui/game_ui/ui_data_bind.hpp/.cpp` | {key} templates, if directives, class: bindings, bar fill |
| `src/ui/game_ui/ui_animation.hpp/.cpp` | Transition property interpolation with easing |
| `src/ui/game_ui/ui_context.hpp` | Game-to-UI data bridge |
| `src/ui/game_ui/ui_renderer.hpp/.cpp` | Vulkan SSBO quad + text renderer |
| `src/ui/game_ui/font_atlas.hpp/.cpp` | MSDF font atlas via FreeType + msdfgen |
| `src/ui/game_ui/text_shaper.hpp/.cpp` | Text layout with kerning and word-wrap |
| `src/ui/game_ui/ui_watcher.hpp/.cpp` | inotify hot-reload (debug only) |
| `src/ui/game_ui/game_ui.hpp/.cpp` | Orchestrator (load, update, record) |
| `shaders/ui_quad.vert/.frag` | Quad pipeline (SDF rounded rect, gradient, shadow) |
| `shaders/ui_text.vert/.frag` | Text pipeline (MSDF sampling) |
| `assets/ui/` | .fjui and .fjss files |
| `assets/fonts/` | TTF fonts |
