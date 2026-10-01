#ifndef FJELL_WATER_MESH_GLSL
#define FJELL_WATER_MESH_GLSL

// What the water's two mesh stages share: water.mesh, which draws the
// surface, and water_depth.mesh, the prepass that writes its depth and
// nothing else. The patch, the water's set as far as placing a vertex reads
// it, the push block, the task payload, and where each vertex lands.
//
// The colour draw tests LESS_OR_EQUAL against the depth the prepass wrote, so
// the two have to put every vertex in the same place to the bit. Both place it
// through fjell_water_vertex(), and gl_Position is invariant, which is what
// promises the same bits from the same expressions in two shaders. Anything
// that moves a vertex goes in here, or the two draws part.

#include "fjell_global_ubo.glsl"
#include "fjell_water_clipmap.glsl"
#include "fjell_water_field.glsl"

// One workgroup emits one water patch. Same arithmetic as terrain.mesh, and
// the same constraint behind the number: primitives bind before vertices in
// the mesh shader output limits — 2N^2 outgrows (N+1)^2 — so 11 quads a side
// (144 verts, 242 prims) is the largest square patch that fits the 256/256
// floor.
//
// The patch's place in the world arrives from the task stage, which is what
// lets the same code generate a patch of a placed square and a patch of a
// clipmap level.
#define PATCH_QUADS 11
#define PATCH_VERTS (PATCH_QUADS + 1)
#define MAX_VERTS   (PATCH_VERTS * PATCH_VERTS)
#define MAX_TRIS    (PATCH_QUADS * PATCH_QUADS * 2)

layout(local_size_x = 64) in;
layout(triangles, max_vertices = MAX_VERTS, max_primitives = MAX_TRIS) out;

FJELL_DECLARE_GLOBAL_UBO(0, 0, ubo);

// Set 2 is the water's own: the wave field it is shaped by. Terrain puts its
// height texture here; water's surface is a formula rather than a texture, so
// what it needs is the parameters.
layout(set = 2, binding = 0) uniform WaterFieldBuffer {
    FjellWaterField field;
} water;

// The evaluators read the field in place, from here.
#define FJELL_WATER_FIELD water.field
#include "fjell_water_waves.glsl"

layout(set = 2, binding = 1) uniform WaterClipmapBuffer {
    FjellWaterClipmap clipmap;
} clip;

// The ground under the water: its terrain's heights, which the sea shoals and
// breaks against. A texel of nothing when the field has no shore, and then
// never read.
layout(set = 2, binding = 7) uniform sampler2D bed_heights;
#define FJELL_WATER_BED(texel) texelFetch(bed_heights, texel, 0).r
// And how open it is to the sea past its land: two amplitude shares a sample,
// the Gerstner trains' and the simulated sea's. A texel of open sea when the
// field has no exposure, and then never read.
layout(set = 2, binding = 8) uniform sampler2D shore_exposure;
#define FJELL_WATER_EXPOSURE(texel) texelFetch(shore_exposure, texel, 0).rg
#include "fjell_water_shore.glsl"

// The simulated sea, when the field has one (field.simulated_patch_size > 0):
// a contributor that arrives as textures because it is an FFT's output rather
// than a formula. Tiling, mipped, and written by WaterFftPass.
//   displacement: xyz offset in world axes, w d(offset x)/dz
// Its previous frame and its derivatives are the colour draw's alone.
layout(set = 2, binding = 2) uniform sampler2D simulated_displacement;

// What has moved through the water: every water node's records for this
// frame, this node's at [push.wake_first, +wake_count). Read in place.
#include "fjell_water_wake_record.glsl"
layout(std430, set = 2, binding = 5) readonly buffer WakeBuffer {
    FjellWakeSource sources[];
} wakes;
#define FJELL_WATER_WAKES wakes.sources
#include "fjell_water_wake.glsl"

layout(push_constant) uniform PushConstants {
    vec2  world_origin;
    float world_size;
    float surface_height;
    uint  patches_per_side;
    uint  patch_quads;
    uint  water_material_id;
    // Seconds since the previous frame. The sea moves under a still camera,
    // so motion vectors need where a vertex was rather than only where the
    // camera was, and the global UBO carries time but no delta.
    float delta_time;
    // Where the sea travels. Unused by the mesh stage; present so the push
    // block matches WaterPushConstants and the fragment stage, which reads it.
    vec2  wave_direction;
    uint  infinite;
    float simulated_patch_size;   // unread here; the field buffer carries it for this stage
    // Unread by the mesh stage; present so the block matches
    // WaterPushConstants and the fragment stage, which samples the scene SDF
    // for contact foam.
    vec3  sdf_volume_min;
    // This node's wakes in the wake buffer.
    uint  wake_first;
    vec3  sdf_volume_max;
    uint  wake_count;
    // Unread by the mesh stage: whether the camera is under this water.
    uint  camera_below;
    uint  line_active;   // the waterline dispatch alone reads it
    uint  _pad_b;
    uint  _pad_c;
} push;

struct TaskPayload {
    vec2  patch_origin;
    float cell_size;
    uint  level;
    uint  coarse_edges;
};

taskPayloadSharedEXT TaskPayload payload;

// Invariant: the prepass and the colour draw compute it in two shaders, and
// it has to come out the same in both.
out gl_MeshPerVertexEXT {
    invariant vec4 gl_Position;
} gl_MeshVerticesEXT[];

// One vertex of the patch, placed: where it rests, what moved it and where
// to. The parts are kept for the colour draw, which carries them on into the
// surface's slopes; the prepass wants the position alone.
struct FjellWaterVertex {
    vec2 rest;               // the flat surface this vertex stands for, seam closed
    bool on_coarse_edge;     // on an edge shared with a coarser level
    FjellShoreEffect shore;  // what the ground does to the sea here
    vec3 trains;             // the closed-form contributors, before the shore
    // The simulated sea's displacement texel and where it was read, when the
    // field has one; zero and unread when not.
    vec4 simulated;
    vec2 simulated_uv;
    float simulated_lod;
    FjellWakeSurface wake;   // the wakes wide enough to move this grid; zero without any
    vec3 offset;             // all of it together: where the vertex moves from rest
};

// The cell size a vertex's wakes are faded out by: the level's own, or on an
// edge shared with a coarser level, that level's.
float fjell_water_wake_cell(FjellWaterVertex v) {
    return v.on_coarse_edge ? 2.0 * payload.cell_size : payload.cell_size;
}

FjellWaterVertex fjell_water_vertex(uint i, float time) {
    FjellWaterVertex v;
    uint ix = i % PATCH_VERTS;
    uint iz = i / PATCH_VERTS;

    // The flat surface this vertex belongs to, before any wave moves it.
    vec2 rest = payload.patch_origin + vec2(ix, iz) * payload.cell_size;

    // Close the seam against a coarser neighbour.
    //
    // The level outside this one samples at twice the spacing, so only
    // this patch's even vertices exist over there. An odd vertex on a
    // shared edge sits on the fine surface, which is not the straight
    // line the coarse edge draws between its neighbours — the gap between
    // them is the crack, and on transparent water it is a slit through to
    // the sky rather than a hairline.
    //
    // Dropping the odd vertex to the midpoint of its two even neighbours
    // puts it exactly on that line, so the two levels meet with no gap at
    // all rather than nearly none. It costs the edge its finest detail,
    // which is the right trade: that detail is what the coarse level
    // could not represent in the first place.
    //
    // Only the vertex's rest position moves. The wave field is still
    // evaluated at it, so the edge is displaced water and not a flat
    // ribbon.
    // Which cell of the level this vertex is, counted from the level's
    // own origin rather than from this patch's corner. A patch is 11
    // cells wide, an odd number, so a vertex's parity within its patch
    // flips from one patch to the next and says nothing about whether the
    // coarse level has a vertex there. Its parity in level cells does.
    ivec2 level_cell = ivec2(round(rest / payload.cell_size));
    bvec2 odd = notEqual(level_cell & 1, ivec2(0));

    // Whether this vertex lies on an edge shared with a coarser level at
    // all, odd or even: it decides which mip of the simulated sea the
    // vertex reads, below.
    v.on_coarse_edge = ((payload.coarse_edges & 1u) != 0u && ix == 0u)
                    || ((payload.coarse_edges & 2u) != 0u && ix == PATCH_QUADS)
                    || ((payload.coarse_edges & 4u) != 0u && iz == 0u)
                    || ((payload.coarse_edges & 8u) != 0u && iz == PATCH_QUADS);

    vec2 seam = rest;
    if ((payload.coarse_edges & 1u) != 0u && ix == 0u && odd.y) {
        seam.y -= payload.cell_size;
    }
    if ((payload.coarse_edges & 2u) != 0u && ix == PATCH_QUADS && odd.y) {
        seam.y -= payload.cell_size;
    }
    if ((payload.coarse_edges & 4u) != 0u && iz == 0u && odd.x) {
        seam.x -= payload.cell_size;
    }
    if ((payload.coarse_edges & 8u) != 0u && iz == PATCH_QUADS && odd.x) {
        seam.x -= payload.cell_size;
    }
    v.rest = seam;

    // What the ground does to the sea here: each sum grows by its own
    // shoaling and both are held to what the depth can carry. The
    // identity where there is no ground near enough to matter.
    v.shore = fjell_water_shore(v.rest);

    // The closed-form contributors, scaled by the shore.
    v.trains = fjell_water_displace(v.rest, time);
    v.offset = v.trains * v.shore.gerstner;

    // The simulated contributor.
    v.simulated = vec4(0.0);
    v.simulated_uv = vec2(0.0);
    v.simulated_lod = 0.0;
    float patch_size = water.field.simulated_patch_size;
    if (patch_size > 0.0) {
        float texels = float(textureSize(simulated_displacement, 0).x);
        // Texel (i, j) holds the patch at (i, j) * patch / texels, and
        // its centre is half a texel in.
        v.simulated_uv = v.rest / patch_size + 0.5 / texels;
        // A vertex standing for more ground than a texel covers reads the
        // sea averaged over that ground, or the far levels would sample
        // one wave in many and shimmer as the camera moves.
        //
        // On an edge shared with a coarser level, the mip is the one that
        // level reads, from its own cell size. The wave field's closed
        // form gives the same answer at a rest position whichever level
        // asks; a texture read at two mips does not, and the two levels
        // would part along the whole edge. Sampling the coarse level's
        // mip on the shared edge puts both sides on one surface, at the
        // cost of that edge's finest detail — which the coarse level could
        // not have shown in any case.
        float cell = v.on_coarse_edge ? 2.0 * payload.cell_size : payload.cell_size;
        v.simulated_lod = log2(max(cell * texels / patch_size, 1.0));
        v.simulated = textureLod(simulated_displacement, v.simulated_uv, v.simulated_lod);
        v.offset += v.simulated.xyz * v.shore.simulated;
    }

    // The wakes wide enough for this grid to draw, vertically only, so
    // they touch the height and its slope and leave the fold measure
    // alone. Faded out by the level's own cell size — on a shared edge by
    // the coarser level's, for the reason the simulated sea's mip is.
    v.wake.height = 0.0;
    v.wake.slope = vec2(0.0);
    if (push.wake_count > 0u) {
        v.wake = fjell_water_wake_surface(push.wake_first, push.wake_count, v.rest, time,
                                          fjell_water_wake_cell(v));
        v.offset.y += v.wake.height;
    }
    return v;
}

// Where a placed vertex is in the world.
vec3 fjell_water_world_position(FjellWaterVertex v) {
    return vec3(v.rest.x + v.offset.x, push.surface_height + v.offset.y, v.rest.y + v.offset.z);
}

// The patch's triangles: fixed grid topology, pure index arithmetic, no index
// buffer.
void fjell_water_patch_triangles() {
    for (uint t = gl_LocalInvocationIndex; t < MAX_TRIS; t += 64) {
        uint quad = t >> 1;
        uint qx = quad % PATCH_QUADS;
        uint qz = quad / PATCH_QUADS;
        uint tl = qz * PATCH_VERTS + qx;
        uint tr = tl + 1;
        uint bl = tl + PATCH_VERTS;
        uint br = bl + 1;
        gl_PrimitiveTriangleIndicesEXT[t] = (t & 1u) == 0u ? uvec3(tl, tr, br)
                                                           : uvec3(tl, br, bl);
    }
}

#endif // FJELL_WATER_MESH_GLSL
