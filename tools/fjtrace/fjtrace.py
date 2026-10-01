#!/usr/bin/env python3
"""fjtrace: capture the engine with Tracy and turn captures into a browser report.

    fjtrace report  base.tracy cur.tracy -o ab.html --open
    fjtrace capture -o cur.tracy -- ./build-tracy/src/fjell ~/test_project --play scene.fjell
    fjtrace tools   [--build-dir build-tracy]

A capture is a `.tracy` file; beside it the tool keeps the CSV exports
(`<stem>.gpu.csv`, `<stem>.cpu.csv`) that `tracy-csvexport` produces and reuses
them while they are newer than the capture. `<stem>.engine.log`, when present,
supplies the window size the run was made at.

The report is one self-contained HTML file: per-frame GPU and CPU time over the
capture, per-zone statistics with deltas against the first capture, a ranked
bar chart of passes, and a flame view of a representative frame. Everything
recomputes in the browser when the analysis window is changed.

Only the Python standard library is needed.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import webbrowser
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / "report.html"

# Durations are embedded as integers in tenths of a microsecond: fine enough for
# a 12 us GPU pass, compact enough to carry every frame of a capture.
UNIT_NS = 100

# Zones that start a frame, in order of preference. Tracy's frame marks do not
# survive the CSV export, so frames are cut at a zone instead. input_update is
# the first zone after FTK_PROFILE_FRAME in the main loop. Mesh Depth is the
# depth prepass, the first pass drawn every frame; the instance cull that can
# precede it is skipped on frames with nothing to cull, so it lands at the tail
# of the previous frame. When none is present, pick_anchor() falls back to the
# once-per-frame zone that follows the largest gap.
CPU_FRAME_ZONES = ("input_update",)
GPU_FRAME_ZONES = ("Mesh Depth",)


def tenths(ns: int) -> int:
    return (ns + UNIT_NS // 2) // UNIT_NS


# --------------------------------------------------------------------------
# Locating the Tracy tools
# --------------------------------------------------------------------------

def tracy_source_dir(build_dir: Path) -> Path | None:
    src = build_dir / "_deps" / "tracy-src"
    return src if (src / "capture").is_dir() else None


def tracy_client_version(build_dir: Path) -> str | None:
    src = tracy_source_dir(build_dir)
    if src is None:
        return None
    text = (src / "public" / "common" / "TracyVersion.hpp").read_text()
    parts = [re.search(rf"{k} = (\d+)", text) for k in ("Major", "Minor", "Patch")]
    if not all(parts):
        return None
    return ".".join(p.group(1) for p in parts)


def find_tool(name: str, explicit: str | None, build_dir: Path) -> Path:
    """Resolve tracy-capture or tracy-csvexport.

    Order: the explicit flag, FJELL_TRACY_TOOLS/<name>, the copy `fjtrace tools`
    built into the engine's build tree (always the protocol the engine speaks),
    then PATH.
    """
    if explicit:
        return Path(explicit)
    env = os.environ.get("FJELL_TRACY_TOOLS")
    if env:
        for candidate in (Path(env) / name, Path(env) / f"{name}.exe"):
            if candidate.is_file():
                return candidate
    short = name.removeprefix("tracy-")
    built = build_dir / "tracy-tools" / short / name
    for candidate in (built, built.with_suffix(".exe")):
        if candidate.is_file():
            return candidate
    on_path = shutil.which(name)
    if on_path:
        return Path(on_path)
    sys.exit(f"fjtrace: {name} not found. Run `fjtrace tools --build-dir {build_dir}` "
             f"to build it from the Tracy source the engine uses, or pass --{short}.")


# Changes made to the Tracy source before the tools are built from it. Each is
# (file, text Tracy has, text to put instead). Text that is not found stops the
# build: a newer Tracy has moved the code, and the change has to be looked at
# again rather than silently dropped.
TRACY_PATCHES = [
    # Slab::AllocBig hands out memory wherever the last allocation ended, so the
    # arrays a trace is loaded into can start at any byte. Tracy builds the tools
    # with -march=native, and the compiler vectorises loops over those arrays
    # with aligned loads: loading a trace whose inline-symbol array happens to
    # start off an 8-byte boundary faults inside the sort that follows
    # (TracyWorker.cpp, symbolLocInline). Rounding the offset up gives every
    # AllocBig block the alignment operator new would. Unalloc() only ever
    # undoes an AllocRaw, so the padding never has to be taken back.
    ("server/TracySlab.hpp",
     "    tracy_force_inline void* AllocBig( size_t size )\n"
     "    {\n"
     "        const auto offset = m_offset;\n"
     "        if( offset + size <= BlockSize )\n"
     "        {\n"
     "            void* ret = m_ptr + offset;\n"
     "            m_offset += size;\n",
     "    tracy_force_inline void* AllocBig( size_t size )\n"
     "    {\n"
     "        constexpr size_t align = __STDCPP_DEFAULT_NEW_ALIGNMENT__;\n"
     "        const auto offset = ( m_offset + align - 1 ) & ~( align - 1 );\n"
     "        if( offset + size <= BlockSize )\n"
     "        {\n"
     "            void* ret = m_ptr + offset;\n"
     "            m_offset = offset + size;\n"),
]


def patched_tracy_source(src: Path, dest: Path) -> Path:
    """Copy the engine's Tracy source to `dest` with TRACY_PATCHES applied.

    The tools are built from this copy, so the source the engine was built
    from stays exactly as it was fetched.
    """
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns(".git"))
    for rel, old, new in TRACY_PATCHES:
        path = dest / rel
        text = path.read_text()
        if old not in text:
            sys.exit(f"fjtrace: {rel} no longer holds the code fjtrace patches; "
                     f"this Tracy version needs TRACY_PATCHES looked at again")
        path.write_text(text.replace(old, new, 1))
    return dest


def configured_source(build: Path) -> Path | None:
    """The source directory a CMake build directory was configured from."""
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:"):
            return Path(line.split("=", 1)[1]).resolve()
    return None


def cmd_tools(args: argparse.Namespace) -> None:
    build_dir = Path(args.build_dir)
    src = tracy_source_dir(build_dir)
    if src is None:
        sys.exit(f"fjtrace: no Tracy source under {build_dir}/_deps; configure the "
                 f"tracy preset first (cmake --preset tracy).")
    out_root = build_dir / "tracy-tools"
    patched = patched_tracy_source(src, out_root / "source")
    for tool in ("capture", "csvexport"):
        out = out_root / tool
        if configured_source(out) not in (None, (patched / tool).resolve()):
            shutil.rmtree(out)  # configured from another source; CMake would refuse it
        print(f"[fjtrace] building tracy-{tool} -> {out}")
        subprocess.run(["cmake", "-S", str(patched / tool), "-B", str(out),
                        "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["cmake", "--build", str(out), "--parallel"], check=True)
    version = tracy_client_version(build_dir)
    print(f"[fjtrace] done; tools match Tracy {version or '(unknown version)'}")


# --------------------------------------------------------------------------
# Capturing
# --------------------------------------------------------------------------

def cmd_capture(args: argparse.Namespace) -> None:
    if not args.cmd:
        sys.exit("fjtrace capture: give the engine command after `--`.")
    build_dir = Path(args.build_dir)
    capture_bin = find_tool("tracy-capture", args.capture, build_dir)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    stem = out.with_suffix("")
    engine_log = stem.with_name(stem.name + ".engine.log")

    print(f"[fjtrace] launching: {' '.join(args.cmd)}")
    print(f"[fjtrace] engine log: {engine_log}")
    popen_kwargs: dict = {}
    if os.name == "posix":
        popen_kwargs["start_new_session"] = True
    else:
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    with open(engine_log, "w") as log:
        engine = subprocess.Popen(args.cmd, stdout=log, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, **popen_kwargs)
        try:
            print(f"[fjtrace] warming up for {args.warmup:g} s")
            deadline = time.monotonic() + args.warmup
            while time.monotonic() < deadline:
                if engine.poll() is not None:
                    sys.exit(f"fjtrace: engine exited during warm-up with code "
                             f"{engine.returncode}; see {engine_log}")
                time.sleep(0.25)
            print(f"[fjtrace] capturing {args.seconds:g} s -> {out}")
            result = subprocess.run([str(capture_bin), "-o", str(out), "-a", args.address,
                                     "-p", str(args.port), "-s", str(args.seconds), "-f"])
            if result.returncode != 0:
                print("[fjtrace] tracy-capture failed. If it reported an incompatible "
                      "protocol version, run `fjtrace tools` to build a matching one.",
                      file=sys.stderr)
                sys.exit(result.returncode)
        finally:
            stop_engine(engine)
    print(f"[fjtrace] capture written: {out}")
    if args.export:
        export_csvs(out, find_tool("tracy-csvexport", args.csvexport, build_dir))


def stop_engine(engine: subprocess.Popen) -> None:
    if engine.poll() is not None:
        return
    if os.name == "posix":
        import signal
        os.killpg(engine.pid, signal.SIGTERM)
    else:
        engine.terminate()
    try:
        engine.wait(timeout=15)
    except subprocess.TimeoutExpired:
        engine.kill()
        engine.wait()


# --------------------------------------------------------------------------
# CSV export and parsing
# --------------------------------------------------------------------------

def sidecar(capture: Path, suffix: str) -> Path:
    return capture.with_name(capture.stem + suffix)


def export_csvs(capture: Path, csvexport: Path) -> tuple[Path, Path]:
    gpu_csv = sidecar(capture, ".gpu.csv")
    cpu_csv = sidecar(capture, ".cpu.csv")
    for out, flags in ((gpu_csv, ["-g", "-u"]), (cpu_csv, ["-u"])):
        if out.is_file() and out.stat().st_mtime >= capture.stat().st_mtime:
            continue
        print(f"[fjtrace] exporting {out.name}")
        with open(out, "w") as f:
            subprocess.run([str(csvexport), *flags, str(capture)], stdout=f, check=True)
    return gpu_csv, cpu_csv


@dataclass
class Event:
    __slots__ = ("start", "dur", "name", "thread")
    start: int
    dur: int
    name: str
    thread: int


def read_gpu_csv(path: Path) -> list[Event]:
    events: list[Event] = []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header or header[0] != "name":
            sys.exit(f"fjtrace: {path} is not a tracy-csvexport GPU export")
        for row in reader:
            events.append(Event(int(row[2]), int(row[3]), row[0], 0))
    events.sort(key=lambda e: (e.start, -e.dur))
    return events


def read_cpu_csv(path: Path) -> dict[int, list[Event]]:
    """Per-thread zone lists, each sorted by start."""
    per_thread: dict[int, list[Event]] = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header or header[0] != "name":
            sys.exit(f"fjtrace: {path} is not a tracy-csvexport CPU export")
        for row in reader:
            thread = int(row[5]) if row[5] else 0
            per_thread[thread].append(Event(int(row[3]), int(row[4]), row[0], thread))
    for events in per_thread.values():
        events.sort(key=lambda e: (e.start, -e.dur))
    return per_thread


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------

def nest(events: list[Event]) -> tuple[list[int], list[int], list[int]]:
    """Depth, parent index and self time for zones sorted by start.

    csvexport flattens the zone tree; interval containment rebuilds it. A zone
    whose start lies inside the zone on top of the stack is its child.
    """
    n = len(events)
    depth = [0] * n
    parent = [-1] * n
    self_time = [e.dur for e in events]
    stack: list[int] = []
    for i, e in enumerate(events):
        while stack and events[stack[-1]].start + events[stack[-1]].dur <= e.start:
            stack.pop()
        if stack:
            p = stack[-1]
            parent[i] = p
            depth[i] = depth[p] + 1
            self_time[p] -= e.dur
        stack.append(i)
    return depth, parent, self_time


def pick_anchor(events: list[Event], depth: list[int], override: str | None,
                preferred: tuple[str, ...]) -> str | None:
    """The top-level zone every frame is cut at.

    An explicit override wins, then the first of `preferred` that occurs about
    once per frame. Failing both, among top-level zone names the modal
    occurrence count is taken as the frame count, and of the names with that
    count the one that follows the largest gap wins: between frames the GPU
    waits for the next submission and the main thread for the swapchain, so
    the frame boundary is the largest idle in the cycle.
    """
    counts = Counter(e.name for e, d in zip(events, depth) if d == 0)
    if override:
        if override not in counts:
            sys.exit(f"fjtrace: frame zone '{override}' is not a top-level zone "
                     f"(top-level zones: {', '.join(sorted(counts))})")
        return override
    if not counts:
        return None
    # A zone cut off at either end of the capture loses its parent and shows up
    # top-level once; those one-offs must not outvote the once-per-frame zones.
    frequent = {name: c for name, c in counts.items() if c >= 10} or counts
    modal = Counter(frequent.values()).most_common(1)[0][0]
    for name in preferred:
        if abs(counts.get(name, 0) - modal) <= 2:
            return name
    candidates = {name for name, c in frequent.items() if c == modal}
    gaps: dict[str, list[int]] = defaultdict(list)
    prev_end = None
    for e, d in zip(events, depth):
        if d != 0:
            continue
        if prev_end is not None and e.name in candidates:
            gaps[e.name].append(e.start - prev_end)
        prev_end = max(prev_end or 0, e.start + e.dur)
    if gaps:
        return max(gaps, key=lambda name: statistics.median(gaps[name]))
    return None


@dataclass
class Frames:
    anchor: str
    starts: list[int]                 # start of every complete frame, plus the next anchor as the end
    t: list[float] = field(default_factory=list)
    period: list[int] = field(default_factory=list)
    busy: list[int] = field(default_factory=list)
    totals: dict[str, list[int]] = field(default_factory=dict)
    selfs: dict[str, list[int]] = field(default_factory=dict)
    counts: dict[str, list[int]] = field(default_factory=dict)
    threads: dict[str, int] = field(default_factory=dict)

    @property
    def count(self) -> int:
        return len(self.starts) - 1


def segment(events: list[Event], depth: list[int], anchor: str) -> list[int]:
    starts = [e.start for e, d in zip(events, depth) if d == 0 and e.name == anchor]
    return starts


def frame_of(starts: list[int], t: int) -> int:
    """Index of the complete frame containing time t, or -1."""
    k = bisect.bisect_right(starts, t) - 1
    if k < 0 or k >= len(starts) - 1:
        return -1
    return k


def accumulate(frames: Frames, events: list[Event], depth: list[int], parent: list[int],
               self_time: list[int], t0: int, key_of, count_top_level: bool) -> None:
    n = frames.count
    frame_index = [-1] * len(events)
    for i, e in enumerate(events):
        if parent[i] >= 0:
            frame_index[i] = frame_index[parent[i]]
        else:
            frame_index[i] = frame_of(frames.starts, e.start)
    if count_top_level:
        frames.t = [(s - t0) / 1e9 for s in frames.starts[:-1]]
        frames.period = [tenths(frames.starts[k + 1] - frames.starts[k]) for k in range(n)]
        frames.busy = [0] * n
    for i, e in enumerate(events):
        k = frame_index[i]
        if k < 0:
            continue
        key = key_of(e)
        totals = frames.totals.get(key)
        if totals is None:
            totals = frames.totals[key] = [0] * n
            frames.selfs[key] = [0] * n
            frames.counts[key] = [0] * n
            frames.threads[key] = e.thread
        totals[k] += e.dur
        frames.selfs[key][k] += self_time[i]
        frames.counts[key][k] += 1
        if count_top_level and depth[i] == 0:
            frames.busy[k] += e.dur


def sample_frames(frames: Frames, events: list[Event], depth: list[int], skip: float) -> dict:
    """Three whole frames for the flame view: median, p95 and the slowest.

    Chosen among the frames after the skip, by their top-level zone total.
    """
    eligible = [k for k in range(frames.count) if frames.t[k] >= skip] or list(range(frames.count))
    if not eligible:
        return {}
    order = sorted(eligible, key=lambda k: frames.busy[k])
    picks = {
        "median": order[len(order) // 2],
        "p95": order[min(len(order) - 1, int(len(order) * 0.95))],
        "worst": order[-1],
    }
    out = {}
    for label, k in picks.items():
        lo, hi = frames.starts[k], frames.starts[k + 1]
        a = bisect.bisect_left([e.start for e in events], lo)
        zones = []
        for i in range(a, len(events)):
            e = events[i]
            if e.start >= hi:
                break
            zones.append([e.name, tenths(e.start - lo), tenths(e.dur), depth[i], e.thread])
        out[label] = {"frame": k, "t": frames.t[k], "busy": tenths(frames.busy[k]),
                      "period": frames.period[k], "zones": zones}
    return out


def finalize_zones(frames: Frames) -> dict:
    zones = {}
    for key, totals in frames.totals.items():
        selfs = frames.selfs[key]
        counts = frames.counts[key]
        entry: dict = {"total": [tenths(v) for v in totals]}
        if any(s != t for s, t in zip(selfs, totals)):
            entry["self"] = [tenths(v) for v in selfs]
        if any(c != 1 for c in counts):
            entry["count"] = counts
        entry["thread"] = frames.threads[key]
        zones[key] = entry
    return zones


def window_size_from_log(capture: Path) -> str | None:
    log = sidecar(capture, ".engine.log")
    if not log.is_file():
        return None
    size = None
    with open(log, errors="replace") as f:
        for line in f:
            m = re.search(r"Window created: (\d+x\d+)", line)
            if m:
                size = m.group(1)
    return size


def analyse(capture: Path, label: str, csvexport: Path, gpu_anchor: str | None,
            cpu_anchor: str | None, skip: float) -> dict:
    print(f"[fjtrace] {label}: {capture}")
    gpu_csv, cpu_csv = export_csvs(capture, csvexport)
    gpu_events = read_gpu_csv(gpu_csv)
    cpu_threads = read_cpu_csv(cpu_csv)
    print(f"[fjtrace]   {len(gpu_events)} GPU zones, "
          f"{sum(len(v) for v in cpu_threads.values())} CPU zones on {len(cpu_threads)} thread(s)")

    firsts = [e.start for e in gpu_events[:1]] + [v[0].start for v in cpu_threads.values() if v]
    t0 = min(firsts) if firsts else 0
    result = {"label": label, "file": str(capture), "window": window_size_from_log(capture),
              "gpu": None, "cpu": None}

    if gpu_events:
        depth, parent, self_time = nest(gpu_events)
        anchor = pick_anchor(gpu_events, depth, gpu_anchor, GPU_FRAME_ZONES)
        if anchor:
            frames = Frames(anchor, segment(gpu_events, depth, anchor))
            if frames.count < 2:
                sys.exit(f"fjtrace: '{anchor}' cuts {capture} into {frames.count} GPU frames; "
                         f"pass --gpu-frame-zone with a zone that starts every frame")
            accumulate(frames, gpu_events, depth, parent, self_time, t0,
                       lambda e: e.name, count_top_level=True)
            result["gpu"] = {
                "anchor": anchor, "t": frames.t, "period": frames.period,
                "busy": [tenths(v) for v in frames.busy],
                "zones": finalize_zones(frames),
                "samples": sample_frames(frames, gpu_events, depth, skip),
            }
            print(f"[fjtrace]   GPU: {frames.count} frames cut at '{anchor}'")

    if cpu_threads:
        main_thread = max(cpu_threads, key=lambda t: len(cpu_threads[t]))
        main = cpu_threads[main_thread]
        depth, parent, self_time = nest(main)
        anchor = pick_anchor(main, depth, cpu_anchor, CPU_FRAME_ZONES)
        if anchor:
            frames = Frames(anchor, segment(main, depth, anchor))
            if frames.count < 2:
                sys.exit(f"fjtrace: '{anchor}' cuts {capture} into {frames.count} CPU frames; "
                         f"pass --cpu-frame-zone with a zone that starts every frame")
            names_per_thread: dict[str, set[int]] = defaultdict(set)
            for thread, events in cpu_threads.items():
                for e in events:
                    names_per_thread[e.name].add(thread)

            def key_of(e: Event) -> str:
                if len(names_per_thread[e.name]) > 1:
                    return f"{e.name} [thread {e.thread}]"
                return e.name

            accumulate(frames, main, depth, parent, self_time, t0, key_of, count_top_level=True)
            all_events = list(main)
            all_depth = list(depth)
            for thread, events in cpu_threads.items():
                if thread == main_thread:
                    continue
                d, p, s = nest(events)
                accumulate(frames, events, d, p, s, t0, key_of, count_top_level=False)
                all_events.extend(events)
                all_depth.extend(d)
            order = sorted(range(len(all_events)), key=lambda i: all_events[i].start)
            all_events = [all_events[i] for i in order]
            all_depth = [all_depth[i] for i in order]
            result["cpu"] = {
                "anchor": anchor, "main_thread": main_thread,
                "t": frames.t, "period": frames.period,
                "busy": [tenths(v) for v in frames.busy],
                "zones": finalize_zones(frames),
                "samples": sample_frames(frames, all_events, all_depth, skip),
            }
            print(f"[fjtrace]   CPU: {frames.count} frames cut at '{anchor}' (thread {main_thread})")
    return result


# --------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------

def resolve_capture(arg: str) -> Path:
    p = Path(arg)
    if p.suffix == ".tracy" and p.is_file():
        return p
    if p.suffix == "" and p.with_suffix(".tracy").is_file():
        return p.with_suffix(".tracy")
    if p.name.endswith(".gpu.csv") or p.name.endswith(".cpu.csv"):
        stem = p.name.rsplit(".", 2)[0]
        tracy = p.with_name(stem + ".tracy")
        if tracy.is_file():
            return tracy
        sys.exit(f"fjtrace: {p} has no {tracy.name} beside it; point at the capture")
    sys.exit(f"fjtrace: no capture at {arg}")


def cmd_report(args: argparse.Namespace) -> None:
    if len(args.captures) > 8:
        sys.exit("fjtrace report: at most eight captures per report.")
    captures = [resolve_capture(c) for c in args.captures]
    labels = list(args.label or [])
    if len(labels) > len(captures):
        sys.exit("fjtrace report: more labels than captures.")
    labels += [c.stem for c in captures[len(labels):]]
    build_dir = Path(args.build_dir)
    csvexport = find_tool("tracy-csvexport", args.csvexport, build_dir)

    runs = [analyse(c, label, csvexport, args.gpu_frame_zone, args.cpu_frame_zone, args.skip)
            for c, label in zip(captures, labels)]
    sizes = {r["window"] for r in runs if r["window"]}
    if len(sizes) > 1:
        print(f"[fjtrace] WARNING: captures were made at different window sizes "
              f"({', '.join(sorted(sizes))}); pixel-bound passes will not compare.")

    data = {
        "generated": time.strftime("%Y-%m-%d %H:%M"),
        "skip": args.skip,
        "unit_us": UNIT_NS / 1000.0,
        "runs": runs,
    }
    template = TEMPLATE.read_text()
    marker = "/*__FJTRACE_DATA__*/"
    if marker not in template:
        sys.exit(f"fjtrace: {TEMPLATE} lacks the data marker")
    payload = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    html = template.replace(marker, payload, 1)

    out = Path(args.output) if args.output else captures[0].with_suffix(".html")
    out.write_text(html)
    print(f"[fjtrace] report: {out} ({out.stat().st_size / 1e6:.1f} MB)")
    if args.open:
        webbrowser.open(out.resolve().as_uri())


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="fjtrace", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--build-dir", default="build-tracy",
                        help="engine build tree with Tracy enabled (default: build-tracy)")
    common.add_argument("--csvexport", help="path to tracy-csvexport")

    p = sub.add_parser("report", parents=[common], help="analyse captures into an HTML report")
    p.add_argument("captures", nargs="+", help=".tracy files; the first is the baseline")
    p.add_argument("-o", "--output", help="report path (default: beside the first capture)")
    p.add_argument("-l", "--label", action="append", help="label for each capture, in order")
    p.add_argument("--skip", type=float, default=4.0,
                   help="seconds to drop from the start of every capture (default: 4)")
    p.add_argument("--gpu-frame-zone", help="top-level GPU zone that starts a frame")
    p.add_argument("--cpu-frame-zone", help="top-level CPU zone that starts a frame")
    p.add_argument("--open", action="store_true", help="open the report in the browser")
    p.set_defaults(func=cmd_report)

    p = sub.add_parser("capture", parents=[common], help="run the engine and capture it")
    p.add_argument("-o", "--output", required=True, help="capture path (.tracy)")
    p.add_argument("--seconds", type=float, default=20, help="capture length (default: 20)")
    p.add_argument("--warmup", type=float, default=25,
                   help="seconds to let the engine load before connecting (default: 25)")
    p.add_argument("--address", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8086)
    p.add_argument("--capture", help="path to tracy-capture")
    p.add_argument("--export", action="store_true", help="export the CSVs right away")
    p.add_argument("cmd", nargs=argparse.REMAINDER, help="engine command after --")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("tools", parents=[common],
                       help="build tracy-capture and tracy-csvexport matching the engine")
    p.set_defaults(func=cmd_tools)

    args = parser.parse_args(argv)
    if args.command == "capture" and args.cmd[:1] == ["--"]:
        args.cmd = args.cmd[1:]
    args.func(args)


if __name__ == "__main__":
    main()
