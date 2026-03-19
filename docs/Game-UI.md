# Game UI

The game UI system renders in-game HUD, menus, and overlays — separate from the ImGui editor chrome. You author UI using markup (`.fjui`) and stylesheets (`.fjss`). Layout is computed via Facebook's Yoga flexbox engine. The result is rendered as Vulkan quads on top of the 3D scene.

## Quick Start

Create two files in `assets/ui/`:

**`assets/ui/test.fjui`**
```html
<panel id="hud">
    <div class="health-bar">
        <div class="health-fill"/>
    </div>
</panel>
```

**`assets/ui/test.fjss`**
```css
#hud {
    flex-direction: column;
    padding: 16;
}

.health-bar {
    width: 200;
    height: 20;
    background-color: #333333;
    border-radius: 4;
}

.health-fill {
    width: 140;
    height: 20;
    background-color: #44bb44;
    border-radius: 4;
}
```

The engine loads `assets/ui/test.fjui` + `assets/ui/test.fjss` automatically at startup. A green health bar appears in the top-left of the viewport.

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
- Other attributes are stored on the node and accessible to later phases (binding, interactivity)
- Tag names are freeform — `div`, `panel`, `slot`, whatever makes sense for your UI

### Node Types

| Type | Description |
|------|-------------|
| Element | Any named tag. Participates in flexbox layout. |
| Text | Raw text between tags. Currently stored but not rendered (text rendering is a future phase). |
| Root | Implicit wrapper if the document has multiple top-level elements. |

## .fjss Stylesheets

CSS-like syntax with selectors and property declarations.

```css
/* Comments */
tag {
    property: value;
}

.class {
    property: value;
}

#id {
    property: value;
}

/* Descendant combinator */
panel .slot {
    property: value;
}
```

### Selectors

| Selector | Specificity | Example |
|----------|-------------|---------|
| `tag` | 1 | `div { ... }` |
| `.class` | 10 | `.panel { ... }` |
| `#id` | 100 | `#hud { ... }` |
| `ancestor descendant` | sum | `panel .slot { ... }` |

Higher specificity wins. Equal specificity: last rule wins.

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
- `padding: 8` → all sides
- `padding: 8 16` → vertical horizontal
- `padding: 8 16 12` → top horizontal bottom
- `padding: 8 16 12 4` → top right bottom left

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
| `background-color` | hex color, named color | Fill color |
| `border-color` | hex color, named color | Border stroke color |
| `border-width` | pixels | Border thickness |
| `border-radius` | 1–4 values (px) | Corner rounding (tl, tr, br, bl) |
| `opacity` | 0.0–1.0 | Overall transparency |

### Colors

```css
background-color: #ff6600;       /* 6-digit hex */
background-color: #ff660080;     /* 8-digit hex (with alpha) */
background-color: #f60;          /* 3-digit shorthand */
background-color: transparent;
background-color: white;
background-color: red;
```

Named colors: `transparent`, `white`, `black`, `red`, `green`, `blue`.

## How It Works

```
.fjui file → FjuiParser → UiNode tree
.fjss file → FjssParser → Stylesheet (rules + selectors)

Per frame:
  StyleResolver  →  matches selectors, applies properties → UiComputedStyle per node
  UiLayout       →  builds Yoga tree, runs flexbox, reads back pixel rects
  UiRenderer     →  flattens tree to GpuUiQuad array, uploads to SSBO
  Vulkan draw    →  6 vertices per quad, SDF rounded rect in fragment shader
```

### Rendering Details

The UI renderer follows the same pattern as the sprite renderer:
- SSBO-based instanced quads (no vertex buffers)
- Double-buffered for CPU/GPU overlap
- 112 bytes per quad (rect, color, border, corners, clip rect, viewport size)
- Vertex shader generates quad corners from `gl_VertexIndex`
- Fragment shader uses SDF for anti-aliased rounded rectangles and borders
- Alpha blending, no depth test (UI always on top)
- Renders after tone mapping, before ImGui

### Frame Graph Integration

The `game_ui` pass runs after `tonemap` and before the debug visualization passes. It renders into the resolved (single-sample) color target with `VK_ATTACHMENT_LOAD_OP_LOAD` so the 3D scene is preserved underneath.

## UiContext

The `UiContext` is the bridge between game code and the UI. Game logic sets values; the UI reads them.

```cpp
auto& ctx = vulkan_context_.game_ui().context();
ctx.set("player.name", "Fjell");
ctx.set_int("player.health", 85);
ctx.set_float("player.stamina", 0.7f);
ctx.bind_action("inventory_open", [&]() { open_inventory(); });
```

Data binding (`{player.health}` in markup) and action dispatch (`on:click`) are planned for a future phase.

## File Locations

| Path | Description |
|------|-------------|
| `src/ui/game_ui/ui_types.hpp` | Enums, UiColor, UiRect, UiEdges, UiCorners |
| `src/ui/game_ui/ui_style.hpp` | UiComputedStyle — all layout + visual properties |
| `src/ui/game_ui/ui_node.hpp` | UiNode — DOM tree node |
| `src/ui/game_ui/fjui_parser.hpp/.cpp` | .fjui markup parser |
| `src/ui/game_ui/fjss_parser.hpp/.cpp` | .fjss stylesheet parser |
| `src/ui/game_ui/style_resolver.hpp/.cpp` | Selector matching and property application |
| `src/ui/game_ui/ui_layout.hpp/.cpp` | Yoga flexbox layout bridge |
| `src/ui/game_ui/ui_context.hpp` | Game-to-UI data bridge |
| `src/ui/game_ui/ui_renderer.hpp/.cpp` | Vulkan SSBO quad renderer |
| `src/ui/game_ui/game_ui.hpp/.cpp` | Orchestrator (load, update, record) |
| `src/ui/game_ui/yoga/` | Vendored Facebook Yoga (MIT license) |
| `shaders/ui_quad.vert` | Quad vertex shader (SSBO → NDC) |
| `shaders/ui_quad.frag` | SDF rounded rect fragment shader |
| `assets/ui/` | .fjui and .fjss files |

## Planned Features

| Phase | Feature |
|-------|---------|
| Text rendering | MSDF font atlas via FreeType + msdfgen, `<text>` elements |
| Full CSS cascade | Shorthand properties, `%` units, gradients, box shadows, inherited properties |
| Interactivity | Mouse hit testing, hover/active states, `{key}` data binding, `on:click` actions, `if`/`each` directives |
| Polish | CSS transitions, scroll containers, `<image>` elements, `<bar>` sugar, .fjui/.fjss hot-reload |
