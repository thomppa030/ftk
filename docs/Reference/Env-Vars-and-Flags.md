# Env Vars & Flags

Runtime knobs that are not surfaced in the editor UI: environment
variables, command line arguments, build defines and debug toggles.

The environment variables and the CLI arguments below are complete. The
build defines and editor-side toggles are still a sketch.

## Environment variables

Every one of these is read once through `getenv`. Unless noted, any value
other than empty or `0` turns the knob on.

| Variable | What it does |
|---|---|
| `FJELL_LOG_LEVEL` | The lowest level that reaches the log, for one run and without a rebuild. One of `trace`, `debug`, `info`, `warn`, `error`; anything else is ignored and the default `info` stands. Debug and trace lines are off by default because they are for chasing one specific problem. |
| `FJELL_TRACE_LAYOUT` | Logs every image barrier the framegraph emits, with the image's handle and name, the layouts it moved between, and the queue. The first thing to reach for on a layout-mismatch validation error. |
| `FJELL_LOG_BARRIERS` | A file to write every barrier the framegraph emits to, for three frames starting `FJELL_LOG_BARRIERS_FRAME` frames in (300 when unset): each pass, and each image and buffer barrier by the name it was declared under, with layouts and stage and access masks. Two runs of one build write the same file, so the traces of two builds diff to what a change did to synchronisation. |
| `FJELL_LOG_LIFETIMES` | Logs each framegraph image's first and last use, and the alias groups derived from them. Tells you why two images did or did not share memory. |
| `FJELL_VALIDATE_ALIASING` | Re-derives the lifetimes and checks that no two images sharing memory are ever live at once. Off by default because it costs work every frame; on when changing the aliasing code. |
| `FJELL_NET_MODE` | Seeds the launch mode for headless and scripted runs. `host` or `join`; any other value is ignored. The editor's Net dropdown overrides it before Play. |

`FJELL_SKY_CUBEMAP` and `FJELL_UI_WORLD` look like the same thing but are
not environment variables. They are preprocessor defines `PipelineCache`
passes to `glslc` to compile a second variant of one `.fjsl` — the sky's
lighting-cubemap compute shader and the world-space UI fragment shader.
Setting them in your shell does nothing.

## CLI args

```
./build/src/fjell [<project-path>] [--scene <name>] [--play] [--create-project <name>]
                  [--run-tests [filter]] [--pacing <list>]
```

| Argument | What it does |
|---|---|
| *(none)* | Opens the hub launcher. |
| `<project-path>` | Opens that project directly, skipping the hub. Any argument that is not one of the flags below is taken as the path, so the last such argument wins. |
| `--scene <name>` | Opens this scene instead of the project's start scene. |
| `--play` | Enters play mode as soon as the project is open. Pairs with `--scene` to launch straight into one scene. |
| `--create-project <name>` | Creates a project of that name before opening. |
| `--run-tests [filter]` | Plays the project's functional tests (scenes under `Tests/`, only those whose name contains the filter if one is given), prints a report and exits non-zero unless every test passed. Editor build only. See [Functional Tests](../Scripting/Functional-Tests.md). |
| `--pacing <list>` | With `--run-tests`: plays every test once per pacing — frame rates and `jitter`, comma separated, as in `30,144,jitter` — and fails a run whose simulation steps differently from the first pacing's. See [Pacing](../Scripting/Functional-Tests.md#pacing). |

`--scene` and `--create-project` each consume the argument after them, and
are ignored if nothing follows.

## Still to write

- **Build defines** — which systems are debug-only (shader, FJUI and
  framegraph hot-reload all are), what a release build strips, and the
  profiler toggle.
- **Editor-side debug toggles** — the visualization passes, the render stat
  overlays and the validation-error gating.
