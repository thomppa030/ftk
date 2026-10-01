# Profiling

The engine carries [Tracy](https://github.com/wolfpld/tracy) zones on the CPU
side (`FTK_PROFILE_SCOPE_N`) and around every render pass on the GPU
(`FJELL_GPU_ZONE`). The `tracy` preset builds the client in; the Tracy GUI is
the way to look at one frame in detail. For the question that comes up far more
often, *what does each pass cost and did my change move it*, `tools/fjtrace`
captures the engine from the shell and turns captures into a report you read in
the browser.

## Build

```bash
cmake --preset tracy
cmake --build --preset tracy
```

`build-tracy/` is RelWithDebInfo with `TRACY_ON_DEMAND`, so the engine runs at
full speed and records only while a capture tool is connected. Rebuild it
before measuring: a stale tree measures stale code. Call-stack sampling is off
(`TRACY_NO_SAMPLING`): captures are read for their zones, and sampling only
added some 300 thousand symbols to every trace and CPU time to the run.

The capture and export tools have to speak the same protocol as the client the
engine was built with, and a distribution's `tracy-capture` usually does not.
Build the pair from the source the engine already fetched:

```bash
python3 tools/fjtrace/fjtrace.py tools
```

They land in `build-tracy/tracy-tools/` and `fjtrace` finds them there before
looking on `PATH`. They are built from a copy of that source with the changes
in `TRACY_PATCHES` (`fjtrace.py`) applied. One is needed today: Tracy's slab
allocator leaves the arrays a trace is loaded into unaligned, and the tools,
built with `-march=native`, crashed loading some traces (exit 139 from
`tracy-csvexport`, on the same trace every time). A newer Tracy that moves the
patched code stops `fjtrace tools` with a message rather than dropping the fix.

## Capture

```bash
python3 tools/fjtrace/fjtrace.py capture -o build-tracy/_captures/base.tracy \
    -- ./build-tracy/src/fjell ~/test_project --scene scenes/water_test.fjell --play
```

This launches the engine, waits for it to load (`--warmup`, 25 s by default),
records for `--seconds` (20) and stops the engine. The engine's output goes to
`base.engine.log` beside the capture, which is also where the report reads the
window size from. Anything after `--` is the engine command line.

Two things decide whether two captures compare at all:

- **Same window size.** Half the passes scale with pixel count. The report
  warns when captures were made at different sizes; that warning means the
  numbers are not a comparison.
- **Same camera.** Frame the shot the same way in both runs, or use a scene
  that starts where you want it. The opening seconds of a run are usually
  spent getting there, which is why the report drops them (`--skip`, 4 s).

## Report

```bash
python3 tools/fjtrace/fjtrace.py report base.tracy cur.tracy -l before -l after --open
```

The first capture is the baseline; every other one is shown against it. Up to
eight captures fit in one report. The tool exports `<stem>.gpu.csv` and
`<stem>.cpu.csv` next to each capture with `tracy-csvexport` and keeps them
while they are newer than the capture, so a second report over the same files
is quick.

The report is one HTML file with no dependencies:

- **GPU time per frame** and **CPU frame period** over the capture. Drag across
  either to set the analysis window; every table and chart below recomputes.
- **GPU passes**: median, p95 and occurrences per frame for every pass and
  capture, with the delta against the baseline. Deltas past the threshold
  (3 % by default) are coloured. *Copy as text* puts the table on the
  clipboard in a form that pastes into a plan or a commit message.
- **Zone over time**: click any row to see that zone frame by frame in every
  capture, which tells a steady shift from a drift or a hitch.
- **Frame timeline**: a flame view of one whole frame, GPU or CPU, for the
  median, p95 or slowest frame after the skip. Wheel zooms, drag pans.
- **CPU zones**: the same table for the CPU, per thread, ranked by self time
  (the zone minus its children) or total.

### Reading the numbers

`FJELL_GPU_ZONE` measures GPU execution; that is a pass's cost. The CPU zone
with a similar name (`record_water`) measures how long the CPU took to record
the commands and says nothing about the GPU.

GPU time per frame is the sum of top-level GPU zones. Passes that overlap on
the async compute queue count in full, so it can exceed the frame period.

Medians are robust to hitches; p95 shows them. When the median moves and p95
does not, the change is in the steady state. When only p95 moves, it is in the
outliers, and the zone-over-time chart shows where.

**Noise.** Two captures of the same build do not give the same numbers. Before
reading anything into a delta, run the baseline twice and report those two
against each other: the largest delta they show is the floor, and a change
below it is not a result. The `Δ` colouring is a threshold, not a
significance test.

### Frames without frame marks

`tracy-csvexport` drops Tracy's frame marks, so the report cuts frames at a
zone instead: `input_update` on the CPU (the first zone after
`FTK_PROFILE_FRAME` in the main loop) and `Mesh Depth` on the GPU (the depth
prepass, drawn every frame; an instance cull before it is skipped on frames
with nothing to cull and so counts at the tail of the previous frame). If a
capture lacks them, the once-per-frame zone that follows the largest gap is
used, and `--cpu-frame-zone` / `--gpu-frame-zone` override either. The chips at the top
say which zone each capture was cut at; two captures cut at different zones
still compare per pass, but their frame charts are offset by part of a frame.
