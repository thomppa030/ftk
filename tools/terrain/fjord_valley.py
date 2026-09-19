#!/usr/bin/env python3
"""Generate a Norwegian fjord-valley .fjterrain.

1024 m square. Steep mountain walls on north, east and west; the south side
opens to the sea. The main river enters the north wall and runs the length of
the valley to the sea. Three springs rise separately in the northeast
mountains, each cutting its own gully, and gather into one creek that joins
the main river partway down. The land between is rolling but buildable,
rising to meet the walls.

Writes the .fjterrain binary directly (FJTERRAIN_VERSION 3, unorm16 heights,
RGBA layer weights) as defined in src/renderer/resources/fjterrain.hpp.
"""

import struct
import sys
import numpy as np

# ---------------------------------------------------------------- parameters

SAMPLES = 2047        # 2046 quads = 186 patches of 11
PATCH_QUADS = 11
WORLD_SIZE = 1024.0   # metres per side
HEIGHT_OFFSET = -20.0 # world height of a normalised 0.0 sample (sea bed)
HEIGHT_SCALE = 250.0  # world height of a normalised 1.0 sample
SEED = 7

MPS = WORLD_SIZE / (SAMPLES - 1)   # metres per sample (~0.5)

def w2n(metres):
    """World metres -> normalised [0,1] sample value."""
    return (metres - HEIGHT_OFFSET) / HEIGHT_SCALE

# ------------------------------------------------------------------- noise

def _hash2(ix, iy, seed):
    h = np.full(ix.shape, np.uint32(seed), dtype=np.uint32)
    h ^= ix.astype(np.uint32) * np.uint32(0x8DA6B343)
    h ^= iy.astype(np.uint32) * np.uint32(0xD8163841)
    h ^= h >> np.uint32(15)
    h *= np.uint32(0x2C1B3C6D)
    h ^= h >> np.uint32(12)
    h *= np.uint32(0x297A2D39)
    h ^= h >> np.uint32(15)
    return (h & np.uint32(0xFFFFFF)).astype(np.float32) / np.float32(0x1000000)


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def gradient_noise(x, y, seed):
    """Perlin-style gradient noise in [0,1]. Same construction as fjterraingen."""
    fx = np.floor(x)
    fy = np.floor(y)
    ix = fx.astype(np.int64)
    iy = fy.astype(np.int64)
    dx = (x - fx).astype(np.float32)
    dy = (y - fy).astype(np.float32)
    tx = _smooth(dx)
    ty = _smooth(dy)

    def corner(cx, cy, ox, oy):
        angle = _hash2(ix + cx, iy + cy, seed) * np.float32(6.283185307)
        return np.cos(angle) * (dx - ox) + np.sin(angle) * (dy - oy)

    d00 = corner(0, 0, 0.0, 0.0)
    d10 = corner(1, 0, 1.0, 0.0)
    d01 = corner(0, 1, 0.0, 1.0)
    d11 = corner(1, 1, 1.0, 1.0)

    top = d00 + (d10 - d00) * tx
    bot = d01 + (d11 - d01) * tx
    return np.clip((top + (bot - top) * ty) * 0.7071 + 0.5, 0.0, 1.0)


def fbm(x, y, seed, octaves, gain=0.45):
    total = np.zeros(x.shape, dtype=np.float32)
    amp = 1.0
    freq = 1.0
    norm = 0.0
    for o in range(octaves):
        total += gradient_noise(x * freq, y * freq, seed + o) * amp
        norm += amp
        amp *= gain
        freq *= 2.0
    return total / norm


def ridged(x, y, seed, octaves, gain=0.45):
    """Folded noise -- creased ridge lines, what makes rock read as rock."""
    total = np.zeros(x.shape, dtype=np.float32)
    amp = 1.0
    freq = 1.0
    norm = 0.0
    for o in range(octaves):
        n = gradient_noise(x * freq, y * freq, seed + o)
        r = 1.0 - np.abs(n * 2.0 - 1.0)
        total += r * r * amp
        norm += amp
        amp *= gain
        freq *= 2.0
    return total / norm


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ------------------------------------------------------------ world sampling

# u runs west->east, v runs north->south. The sea is at v = 1 (south).
u = np.linspace(0.0, 1.0, SAMPLES, dtype=np.float32)
v = np.linspace(0.0, 1.0, SAMPLES, dtype=np.float32)
U, V = np.meshgrid(u, v)          # [row=z][col=x], row-major as the format wants
X = U * WORLD_SIZE                # metres east
Z = V * WORLD_SIZE                # metres south


# ------------------------------------------------------- river centre lines

# A river's course is built along its length as a 1-D function of z, then
# broadcast across the grid. Noise rather than stacked sines: a sine pair
# repeats at a fixed wavelength and reads as a mechanical wave, where octaves
# of noise give long bends carrying short kinks, which is what a real course
# looks like.
Zline = np.linspace(0.0, WORLD_SIZE, SAMPLES, dtype=np.float32)


def wander(zline, seed, scale, octaves=4, cells=3.4):
    """Lateral wander in metres along a course, centred on zero."""
    t = zline / WORLD_SIZE * cells
    n = fbm(t, np.full_like(t, 0.5), seed, octaves)
    return (n - 0.5) * 2.0 * scale


# The main river: enters the north wall west of centre and works its way to
# the sea. Its wander is scaled by how far down the valley it is, so it leaves
# the gorge fairly straight and meanders more as the floor flattens out --
# rivers braid where the grade eases, not where they are still falling.
MAIN_MOUTH_X = 430.0
main_prog = np.clip(Zline / WORLD_SIZE, 0.0, 1.0)
main_freedom = smoothstep(0.10, 0.62, main_prog)
main_line = (MAIN_MOUTH_X
             + 42.0 * main_prog                      # general drift east
             + wander(Zline, SEED + 1301, 95.0, 4) * main_freedom
             + wander(Zline, SEED + 1607, 26.0, 3, cells=9.0) * main_freedom)

# A meander belt: an extra slow swing that makes the lower river swing wide
# across its own floodplain rather than merely jittering about a centre.
main_line += 60.0 * np.sin(main_prog * 4.1 + 0.7) * smoothstep(0.45, 0.95, main_prog)

main_x = np.broadcast_to(main_line[:, None], (SAMPLES, SAMPLES))


# ---------------------------------------------------------------- the springs

# The small river is not one channel from the wall: three springs rise
# separately in the northeast mountains, each cutting its own steep gully down
# the slope, and they gather into one creek that then runs to the main river.
#
# Each spring is (x, z) in metres -- where it surfaces -- and a strength.
# Each spring is (x, z) where it surfaces, a strength, and the z at which it
# is absorbed -- staggered, so the three join one after another down the slope
# instead of all arriving at one tidy point.
SPRINGS = [
    (912.0, 140.0, 1.00, 300.0),
    (838.0, 232.0, 0.85, 356.0),
    (757.0, 119.0, 0.70, 262.0),
]

# The springs gather here, and from this point on there is a single creek.
GATHER_X = 800.0
GATHER_Z = 330.0

# The creek below the gathering point, down to where it meets the main river.
CONFLUENCE_Z = 560.0
CREEK_START_Z = 262.0
creek_prog = np.clip((Zline - CREEK_START_Z) / (CONFLUENCE_Z - CREEK_START_Z), 0.0, 1.0)
# Steer from the gathering point onto whatever the main river is doing, with a
# smoothstep so the creek arrives tangentially instead of turning a corner.
creek_line = (GATHER_X * (1.0 - smoothstep(0.0, 1.0, creek_prog))
              + main_line * smoothstep(0.0, 1.0, creek_prog)
              + wander(Zline, SEED + 1901, 26.0, 4, cells=6.0)
                * smoothstep(0.0, 0.45, creek_prog) * (1.0 - creek_prog))
creek_x = np.broadcast_to(creek_line[:, None], (SAMPLES, SAMPLES))

# The creek exists only between the gathering point and the confluence.
creek_life = (smoothstep(CREEK_START_Z - 20.0, CREEK_START_Z + 30.0, Z)
              * (1.0 - smoothstep(CONFLUENCE_Z - 35.0, CONFLUENCE_Z + 15.0, Z)))


def channel(dist, half_width, depth, bank):
    """Cut profile for a river: flat-ish bed, then banks easing back to grade."""
    bed = 1.0 - smoothstep(0.0, half_width, dist)
    flank = 1.0 - smoothstep(half_width, half_width + bank, dist)
    return depth * np.maximum(bed, flank * 0.55)


# The main river widens and deepens as it nears the sea; below the confluence
# it carries both catchments, so it steps up again there. The width also
# breathes along its length -- pools and narrows -- so the channel is not a
# constant-section ditch.
gain = smoothstep(0.0, 1.0, V)
joined = smoothstep(CONFLUENCE_Z - 40.0, CONFLUENCE_Z + 60.0, Z)
breathe = fbm(Zline / WORLD_SIZE * 7.0, np.full_like(Zline, 2.5), SEED + 2203, 3)
breathe = np.broadcast_to(((breathe - 0.5) * 2.0)[:, None], (SAMPLES, SAMPLES))
main_width = 15.0 + 26.0 * gain + 9.0 * joined + 9.0 * breathe
main_depth = 11.0 + 7.0 * gain + 3.5 * joined + 2.0 * breathe
main_cut = channel(np.abs(X - main_x), main_width, main_depth, 60.0)

# Each spring cuts its own gully from where it surfaces down to the gathering
# point. The gully is a straight-ish run steered by a little noise, narrow and
# steep near the source and easing as it loses height -- a mountain stream.
def spring_gully(sx, sz, strength, join_z):
    # Parameter along the run from the spring to the gathering point.
    t = np.clip((Z - sz) / max(join_z - sz, 1.0), 0.0, 1.0)
    centre = sx + (GATHER_X - sx) * smoothstep(0.0, 1.0, t)
    centre = centre + wander(Zline, SEED + int(sx), 18.0, 3, cells=7.0)[:, None] * t * (1.0 - t) * 4.0
    # Alive only between the spring and the gathering point; it emerges rather
    # than starting at full size, which is what makes it read as a source.
    alive = (smoothstep(sz - 14.0, sz + 26.0, Z)
             * (1.0 - smoothstep(join_z - 26.0, join_z + 12.0, Z)))
    width = (4.5 + 4.5 * t) * strength
    depth = (8.0 + 4.0 * t) * strength
    return channel(np.abs(X - centre), width, depth, 20.0) * alive


spring_cuts = [spring_gully(sx, sz, s, jz) for sx, sz, s, jz in SPRINGS]

# Below the gathering point the three are one creek, larger than any of them.
creek_cut = channel(np.abs(X - creek_x), 6.5 + 7.0 * creek_prog[:, None],
                    8.0 + 4.5 * creek_prog[:, None], 30.0) * creek_life

river_cut = main_cut
for c in spring_cuts:
    river_cut = np.maximum(river_cut, c)
river_cut = np.maximum(river_cut, creek_cut)

# A broad shallow floodplain follows the main river, keeping the ground beside
# it low and flat -- the part of a valley you would actually build on.
floodplain = (1.0 - smoothstep(40.0, 260.0, np.abs(X - main_x))) * 10.0


# ------------------------------------------------------------ valley shaping

# Distance to each closed wall, in metres, as a 0..1 "how far inside" measure.
d_west = X
d_east = WORLD_SIZE - X
d_north = Z
d_south = WORLD_SIZE - Z   # the open side

# Wall onset wanders so the valley is not a rounded rectangle. Each wall gets
# its own noise so the three are not obviously the same curve.
wall_noise_w = fbm(V * 5.0, np.full_like(V, 11.5), SEED + 31, 4)
wall_noise_e = fbm(V * 5.0, np.full_like(V, 71.5), SEED + 57, 4)
wall_noise_n = fbm(U * 5.0, np.full_like(U, 41.5), SEED + 83, 4)

# Metres of valley before each wall starts climbing. Wide enough that the
# massif has room to be a mountain range with shoulders and side spurs,
# rather than a rim around the edge of the map.
WALL_BASE = 230.0
WALL_VARY = 95.0
onset_w = WALL_BASE + WALL_VARY * wall_noise_w
onset_e = WALL_BASE + WALL_VARY * wall_noise_e
onset_n = WALL_BASE + WALL_VARY * wall_noise_n

# Climb hard once past the onset: a steep fjord wall, not a hillside. The
# exponent makes the wall concave -- a gentle apron at the foot where you can
# still build, steepening to near-vertical above, which is how a glacial
# trough actually reads.
def wall(dist, onset, rise):
    t = np.clip(1.0 - dist / onset, 0.0, 1.0)
    return rise * np.power(t, 2.2)


WALL_RISE = 205.0
w_west = wall(d_west, onset_w, WALL_RISE)
w_east = wall(d_east, onset_e, WALL_RISE)
w_north = wall(d_north, onset_n, WALL_RISE)

# The sea side must stay open: no wall, and the ground runs out below zero so
# the shoreline is a real beach rather than a cliff into water.
# The side walls fall away toward the sea so the mouth is genuinely open
# rather than a bay enclosed on three and a half sides.
mouth_open = 1.0 - 0.55 * smoothstep(0.62, 1.0, V)
w_west = w_west * mouth_open
w_east = w_east * mouth_open

walls = np.maximum(np.maximum(w_west, w_east), w_north)

# Soften the corners where two walls meet, so they read as one massif rather
# than two ramps crossing.
corner_blend = (w_west + w_east + w_north) * 0.10
walls = walls + corner_blend * smoothstep(0.0, WALL_RISE * 0.25, walls)


# ---------------------------------------------------------------- rock detail

# Ridged noise on the walls: buttresses and gullies. Scaled by how high the
# wall already is, so the valley floor stays clean.
rock = ridged(U * 7.0, V * 7.0, SEED + 211, 6)
rock_fine = ridged(U * 19.0, V * 19.0, SEED + 307, 5)
wall_mask = smoothstep(8.0, 75.0, walls)
rock_relief = ((rock - 0.5) * 42.0 + (rock_fine - 0.5) * 12.0) * wall_mask

# Snow-line-scale erosion: shallow chutes running down the steepest ground.
chutes = (ridged(U * 13.0, V * 13.0, SEED + 401, 4) - 0.5) * 19.0 * wall_mask


# --------------------------------------------------------------- valley floor

# Rolling buildable ground: large, soft features with broad flats between
# them. Squaring biases toward the lows, so most of the floor is terrace
# rather than slope.
floor_base = fbm(U * 2.6, V * 2.6, SEED + 13, 6)
floor = np.power(floor_base, 1.9) * 26.0

# Glacial benches: the floor steps up in broad terraces rather than ramping
# smoothly, which is what gives a valley distinct places to build on. Quantise
# a slow field, then round the risers off so they are banks and not walls.
bench_field = fbm(U * 2.0, V * 2.4, SEED + 67, 3) * 3.0
bench = np.floor(bench_field)
riser = bench_field - bench
bench_h = (bench + smoothstep(0.62, 0.95, riser)) * 6.5

# Moraine hummocks and knolls: small and frequent, enough to break up the
# ground without ruining buildability.
hummock = (fbm(U * 9.0, V * 9.0, SEED + 137, 4) - 0.5) * 4.5
knoll = np.power(fbm(U * 5.0, V * 5.0, SEED + 179, 3), 3.0) * 13.0

# Side spurs: ridges reaching down from the walls into the valley, so the
# floor is divided into bays rather than being one open field.
spur = np.power(ridged(U * 4.5, V * 3.5, SEED + 223, 4), 2.0)

# The floor tilts gently down toward the sea, which is what gives the rivers
# somewhere to go.
seaward_grade = (1.0 - V) * 22.0

land = floor + bench_h + hummock + knoll + seaward_grade + 6.0


# --------------------------------------------------------------- the sea edge

# Past the southern shore the ground drops below zero into the fjord mouth.
shore_noise = fbm(U * 6.0, np.full_like(U, 3.0), SEED + 509, 4)
shore_z = WORLD_SIZE - (150.0 + 90.0 * shore_noise)
sea_drop = smoothstep(0.0, 150.0, Z - shore_z) * 70.0


# ------------------------------------------------------------------- assemble

# Spurs only exist where a wall is near: they are the wall's own ridges
# running out into the valley, fading before they reach the middle.
spur_reach = smoothstep(0.0, 45.0, walls) * (1.0 - smoothstep(45.0, 140.0, walls))
spur_relief = spur * spur_reach * 34.0

height_m = land + walls + spur_relief + rock_relief + chutes - sea_drop

# Rivers cut last, so they carve through whatever they cross. They enter the
# north wall as gorges and give up only against genuinely high rock, which is
# what makes the mouth read as a notch in the massif rather than a channel
# that starts out of nowhere on the valley floor.
cut_allowed = 1.0 - smoothstep(60.0, 160.0, walls)
height_m = height_m - (river_cut + floodplain * (1.0 - smoothstep(0.0, 40.0, river_cut))) * cut_allowed

# The seabed keeps a gentle slope rather than a flat plate.
seabed = -18.0 + (fbm(U * 4.0, V * 4.0, SEED + 601, 4) - 0.5) * 6.0
height_m = np.maximum(height_m, seabed)

# Peaks compress toward the top of the storable range instead of clipping
# against it: a hard clamp would shear every summit into the same flat plateau.
CEIL = HEIGHT_OFFSET + HEIGHT_SCALE
SOFT = CEIL - 46.0
over = height_m > SOFT
height_m[over] = SOFT + (CEIL - SOFT) * np.tanh((height_m[over] - SOFT) / (CEIL - SOFT))

heights = np.clip(w2n(height_m.astype(np.float32)), 0.0, 1.0)


# -------------------------------------------------------------- layer weights

# Four layers, one per RGBA channel:
#   R sand/shore   G grass/meadow   B rock   A scree
slope_dx = np.gradient(height_m, MPS, axis=1)
slope_dz = np.gradient(height_m, MPS, axis=0)
slope_deg = np.degrees(np.arctan(np.hypot(slope_dx, slope_dz)))

w_sand = (1.0 - smoothstep(1.0, 9.0, height_m)) * (1.0 - smoothstep(22.0, 38.0, slope_deg))
w_sand = np.maximum(w_sand, (1.0 - smoothstep(0.0, 26.0, np.abs(X - main_x))) *
                            (1.0 - smoothstep(14.0, 30.0, height_m)))
w_rock = smoothstep(26.0, 45.0, slope_deg)
w_rock = np.maximum(w_rock, smoothstep(105.0, 150.0, height_m) * 0.8)
w_scree = smoothstep(120.0, 175.0, height_m) * smoothstep(26.0, 40.0, slope_deg) * 1.6
w_grass = np.clip(1.0 - w_sand - w_rock - w_scree, 0.0, 1.0) + 0.05

stack = np.stack([w_sand, w_grass, w_rock, w_scree], axis=-1).astype(np.float32)
stack = np.maximum(stack, 0.0)
total = np.sum(stack, axis=-1, keepdims=True)
stack = stack / np.maximum(total, 1e-6)
weights = np.round(stack * 255.0).astype(np.uint8)


# -------------------------------------------------------------------- write

def write_fjterrain(path, heights, weights, layers, names):
    samples = heights.shape[0]
    weight_count = weights.shape[0] if weights is not None else 0
    header = struct.pack(
        "<IIIIfffIII24x",
        0x5254464A,          # magic "JFTR"
        3,                   # version
        samples,
        0,                   # height_format: unorm16
        WORLD_SIZE,
        HEIGHT_SCALE,
        HEIGHT_OFFSET,
        PATCH_QUADS,
        len(layers),
        weight_count,
    )
    assert len(header) == 64, len(header)

    raw = np.round(np.clip(heights, 0.0, 1.0) * 65535.0).astype("<u2")

    with open(path, "wb") as f:
        f.write(header)
        f.write(raw.tobytes(order="C"))
        if layers and weight_count:
            f.write(weights.tobytes(order="C"))
        if layers:
            for material_idx, uv_scale, surface_idx in layers:
                f.write(struct.pack("<IfI", material_idx, uv_scale, surface_idx))
            f.write(struct.pack("<I", len(names)))
            for n in names:
                b = n.encode("utf-8")
                f.write(struct.pack("<I", len(b)))
                f.write(b)


# The editor allocates two name entries per layer at fixed indices -- material
# at i*2, surface at i*2+1 (TerrainAsset::set_layer_count). Matching that
# convention means opening this terrain and editing a layer does not reshuffle
# the table.
LAYER_SPEC = [
    ("assets/materials/terrain_sand.fjmat",  6.0, "assets/surfaces/sand.fjsurface"),
    ("assets/materials/terrain_grass.fjmat", 4.0, "assets/surfaces/grass.fjsurface"),
    ("assets/materials/terrain_rock.fjmat",  8.0, "assets/surfaces/rock.fjsurface"),
    ("assets/materials/terrain_scree.fjmat", 7.0, "assets/surfaces/scree.fjsurface"),
]
names = []
layers = []
for i, (mat, uv, surf) in enumerate(LAYER_SPEC):
    names += [mat, surf]
    layers.append((i * 2, uv, i * 2 + 1))

out = sys.argv[1] if len(sys.argv) > 1 else "valley.fjterrain"
write_fjterrain(out, heights, weights, layers, names)

hm = height_m
print(f"wrote {out}")
print(f"  {SAMPLES} x {SAMPLES} samples, {(SAMPLES-1)//PATCH_QUADS} patches of {PATCH_QUADS}")
print(f"  world {WORLD_SIZE:.0f}m x {WORLD_SIZE:.0f}m at {MPS:.3f} m/sample")
print(f"  height {hm.min():.1f}m .. {hm.max():.1f}m "
      f"(range {HEIGHT_OFFSET:.0f}..{HEIGHT_OFFSET+HEIGHT_SCALE:.0f})")
clipped_hi = int(np.sum(heights >= 1.0))
clipped_lo = int(np.sum(heights <= 0.0))
print(f"  clipped: {clipped_hi} at ceiling, {clipped_lo} at floor")
