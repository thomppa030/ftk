# Writing Components

> Stub — fill in.

## Scope
How to declare a component, expose fields to the editor, control serialization, validate input. Covers reflection (`fjreflect`), `field_inspector`, and component meta.

## Planned sections
- Subclassing `Component`
- Declaring fields and `FJELL_FIELD(...)` annotations
- Inspector behavior — `field_inspector` & types it supports (numbers, vectors, colors, assets, enums, refs)
- Custom field UI hints (range, color, asset filter, multiline)
- Serialization — what gets saved to `.fjscene`, format guarantees
- Component meta (`*_component_meta.cpp`) — display name, icon, category, validation
- Registering: `FJELL_REGISTER_COMPONENT(MyComponent)`
- Required/forbidden component combos
- Examples: an `InputComponent`, a `SpriteAnimComponent`, a custom one
