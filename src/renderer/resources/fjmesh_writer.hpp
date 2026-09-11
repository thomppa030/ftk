#pragma once

// Writing side of the .fjmesh format. Shared by the fjimport tool and the
// engine, so a mesh written by either is byte-identical in layout.

#include "renderer/resources/fjmesh.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace fjell {

// Pads a file with zeroes up to `target`, for section alignment.
void pad_to_offset(std::FILE* f, uint64_t target);

// The v5 authored sections, laid out and ready to write.
struct AuthoredLayout {
    std::vector<FjMeshMaterialSlot> slots;
    NameTableBuilder names;
};

// Lays out the v5 authored sections at `offset`, filling in the matching
// header fields and returning the offset past them. Call after laying out the
// geometry sections, then pass the layout to write_authored_sections.
uint64_t layout_authored_sections(FjMeshHeader& header, uint64_t offset,
                                  const MeshAuthoredData& authored,
                                  AuthoredLayout& layout);

// Writes the material slot array and name table, padding to each section's
// offset. Call after the geometry sections have been written.
void write_authored_sections(std::FILE* f, const FjMeshHeader& header,
                             const AuthoredLayout& layout);

// Reads just the authored block out of an existing .fjmesh, ignoring
// geometry. Returns an empty result if the file is missing, unreadable, or
// predates v5 — all of which mean "nothing authored to carry over".
[[nodiscard]] MeshAuthoredData read_authored_data(const std::string& path);

// Rewrites only the authored block of an existing .fjmesh, leaving every
// geometry section byte-identical. Used when the editor saves slot
// assignments — the mesh itself hasn't changed, so it isn't rebuilt.
[[nodiscard]] bool rewrite_authored_data(const std::string& path,
                                         const MeshAuthoredData& authored);

// Collision geometry to write in place of what a .fjmesh currently holds.
// `indices` is empty for a convex hull, which is stored as a point cloud.
struct CollisionSectionData {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
};

// Rewrites a .fjmesh's collision geometry along with its authored block.
//
// Unlike rewrite_authored_data, this cannot patch the file in place: the
// collision sections sit between the meshlet data and the LOD table, so
// resizing them moves every section after them. Render geometry is copied
// verbatim and the trailing sections are relocated, which is what lets the
// editor regenerate collision without reimporting the mesh.
[[nodiscard]] bool rewrite_collision_data(const std::string& path,
                                          const CollisionSectionData& collision,
                                          const MeshAuthoredData& authored);

// Locates the source file a .fjmesh was imported from, using the provenance
// recorded at import time. The path is stored relative to the mesh, so moving
// the two together keeps it resolvable. Returns an empty path when the mesh
// has no provenance (any file older than v5) or the source is gone.
[[nodiscard]] std::string find_import_source(const std::string& fjmesh_path);

// Carries authored settings from a previous import onto a fresh one.
// Material slots are matched by source material name rather than index, so
// assignments survive a DCC reordering or inserting materials. `fresh` supplies
// the slot names the new geometry actually has; assignments for names that no
// longer exist are dropped.
void merge_authored_data(MeshAuthoredData& fresh, const MeshAuthoredData& previous);

} // namespace fjell
