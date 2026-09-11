#include "renderer/resources/fjmesh_writer.hpp"

#include <cstddef>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace {

uint64_t align_to(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

void pad_to_offset(std::FILE* f, uint64_t target) {
    long current = std::ftell(f);
    if (current < 0) {
        return;
    }
    while (static_cast<uint64_t>(current) < target) {
        uint8_t zero = 0;
        std::fwrite(&zero, 1, 1, f);
        ++current;
    }
}

uint64_t layout_authored_sections(FjMeshHeader& header, uint64_t offset,
                                  const MeshAuthoredData& authored,
                                  AuthoredLayout& layout) {
    header.collision_source = static_cast<uint32_t>(authored.collision_source);
    header.collision_simplify_ratio = authored.collision_simplify_ratio;
    header.source_modified_time = authored.source_modified_time;

    auto source = layout.names.add(authored.source_file);
    header.source_name_offset = source.first;
    header.source_name_length = source.second;

    auto collision_mesh = layout.names.add(authored.collision_mesh);
    header.collision_mesh_offset = collision_mesh.first;
    header.collision_mesh_length = collision_mesh.second;

    auto uuid = layout.names.add(authored.asset_uuid);
    header.asset_uuid_offset = uuid.first;
    header.asset_uuid_length = uuid.second;

    layout.slots.reserve(authored.material_slots.size());
    for (const auto& slot : authored.material_slots) {
        auto name = layout.names.add(slot.name);
        auto path = layout.names.add(slot.path);
        layout.slots.push_back({name.first, name.second, path.first, path.second});
    }

    header.material_slot_count = static_cast<uint32_t>(layout.slots.size());
    header.material_slots_offset = align_to(offset, 4);
    offset = header.material_slots_offset +
             (header.material_slot_count * sizeof(FjMeshMaterialSlot));

    header.name_table_offset = align_to(offset, 4);
    header.name_table_size = layout.names.size();
    offset = header.name_table_offset + header.name_table_size;

    return offset;
}

void write_authored_sections(std::FILE* f, const FjMeshHeader& header,
                             const AuthoredLayout& layout) {
    pad_to_offset(f, header.material_slots_offset);
    std::fwrite(layout.slots.data(), sizeof(FjMeshMaterialSlot), layout.slots.size(), f);

    pad_to_offset(f, header.name_table_offset);
    std::fwrite(layout.names.data().data(), 1, layout.names.size(), f);
}

MeshAuthoredData read_authored_data(const std::string& path) {
    MeshAuthoredData authored;

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return authored;
    }

    FjMeshHeader header{};
    if (std::fread(&header, sizeof(header), 1, f) != 1 ||
        header.magic != FJMESH_MAGIC || header.version < 5) {
        std::fclose(f);
        return authored;
    }

    authored.collision_source =
        static_cast<FjMeshCollisionSource>(header.collision_source);
    authored.collision_simplify_ratio = header.collision_simplify_ratio;
    authored.source_modified_time = header.source_modified_time;

    std::vector<char> name_table(header.name_table_size);
    if (header.name_table_size > 0) {
        std::fseek(f, static_cast<long>(header.name_table_offset), SEEK_SET);
        if (std::fread(name_table.data(), 1, name_table.size(), f) != name_table.size()) {
            std::fclose(f);
            return {};
        }
    }

    authored.source_file = read_name(name_table, header.source_name_offset,
                                     header.source_name_length);
    authored.collision_mesh = read_name(name_table, header.collision_mesh_offset,
                                        header.collision_mesh_length);
    if (header.version >= 6) {
        authored.asset_uuid = read_name(name_table, header.asset_uuid_offset,
                                        header.asset_uuid_length);
    }

    std::vector<FjMeshMaterialSlot> slots(header.material_slot_count);
    if (header.material_slot_count > 0) {
        std::fseek(f, static_cast<long>(header.material_slots_offset), SEEK_SET);
        if (std::fread(slots.data(), sizeof(FjMeshMaterialSlot), slots.size(), f) !=
            slots.size()) {
            std::fclose(f);
            return {};
        }
    }

    authored.material_slots.reserve(slots.size());
    for (const auto& slot : slots) {
        authored.material_slots.push_back({
            read_name(name_table, slot.name_offset, slot.name_length),
            read_name(name_table, slot.path_offset, slot.path_length)});
    }

    std::fclose(f);
    return authored;
}

bool rewrite_authored_data(const std::string& path, const MeshAuthoredData& authored) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) {
        return false;
    }

    FjMeshHeader header{};
    if (std::fread(&header, sizeof(header), 1, in) != 1 ||
        header.magic != FJMESH_MAGIC) {
        std::fclose(in);
        return false;
    }

    // Everything up to the authored sections is geometry and is copied
    // verbatim; the authored block is always last, so it can simply be
    // replaced. Older files have no authored block and end at the last
    // geometry section, which the same bound covers.
    uint64_t geometry_end = header.version >= 5 && header.material_slots_offset > 0
        ? header.material_slots_offset
        : 0;
    if (geometry_end == 0) {
        std::fseek(in, 0, SEEK_END);
        long size = std::ftell(in);
        if (size < 0) {
            std::fclose(in);
            return false;
        }
        geometry_end = static_cast<uint64_t>(size);
    }

    std::vector<char> body(geometry_end);
    std::fseek(in, 0, SEEK_SET);
    if (std::fread(body.data(), 1, body.size(), in) != body.size()) {
        std::fclose(in);
        return false;
    }
    std::fclose(in);

    // A pre-v5 file gains the new header fields, which shifts every section.
    // Rewriting those offsets means re-laying out the whole file, so leave it
    // to a reimport instead.
    if (header.version < FJMESH_VERSION) {
        return false;
    }

    auto* out_header = reinterpret_cast<FjMeshHeader*>(body.data());
    AuthoredLayout layout;
    layout_authored_sections(*out_header, geometry_end, authored, layout);

    std::string tmp = path + ".tmp";
    std::FILE* out = std::fopen(tmp.c_str(), "wb");
    if (out == nullptr) {
        return false;
    }
    std::fwrite(body.data(), 1, body.size(), out);
    write_authored_sections(out, *out_header, layout);
    std::fclose(out);

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool rewrite_collision_data(const std::string& path,
                            const CollisionSectionData& collision,
                            const MeshAuthoredData& authored) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) {
        return false;
    }

    FjMeshHeader header{};
    if (std::fread(&header, sizeof(header), 1, in) != 1 ||
        header.magic != FJMESH_MAGIC || header.version != FJMESH_VERSION) {
        std::fclose(in);
        return false;
    }

    // Sections before collision keep their offsets, so they are copied as one
    // opaque block rather than being understood.
    uint64_t prefix_end = header.collision_vertices_offset;
    if (prefix_end <= sizeof(FjMeshHeader)) {
        std::fclose(in);
        return false;
    }

    std::vector<char> prefix(prefix_end);
    std::fseek(in, 0, SEEK_SET);
    if (std::fread(prefix.data(), 1, prefix.size(), in) != prefix.size()) {
        std::fclose(in);
        return false;
    }

    // The geometry every rebuild derives from. It is never regenerated here —
    // only carried across — so changing the source stays reversible.
    std::vector<glm::vec3> full_vertices(header.collision_full_vertex_count);
    if (!full_vertices.empty()) {
        std::fseek(in, static_cast<long>(header.collision_full_vertices_offset), SEEK_SET);
        if (std::fread(full_vertices.data(), sizeof(glm::vec3), full_vertices.size(), in) !=
            full_vertices.size()) {
            std::fclose(in);
            return false;
        }
    }
    std::vector<uint32_t> full_indices(header.collision_full_index_count);
    if (!full_indices.empty()) {
        std::fseek(in, static_cast<long>(header.collision_full_indices_offset), SEEK_SET);
        if (std::fread(full_indices.data(), sizeof(uint32_t), full_indices.size(), in) !=
            full_indices.size()) {
            std::fclose(in);
            return false;
        }
    }

    // The LOD table and SDF volume follow collision and are unchanged in
    // content, but their offsets move with it.
    std::vector<char> lod_levels(header.lod_count * sizeof(FjMeshLodLevel));
    if (!lod_levels.empty()) {
        std::fseek(in, static_cast<long>(header.lod_levels_offset), SEEK_SET);
        if (std::fread(lod_levels.data(), 1, lod_levels.size(), in) != lod_levels.size()) {
            std::fclose(in);
            return false;
        }
    }

    std::vector<char> sdf_data;
    if (header.sdf_resolution > 0 && header.sdf_data_offset > 0) {
        auto voxels = static_cast<uint64_t>(header.sdf_resolution) *
                      header.sdf_resolution * header.sdf_resolution;
        sdf_data.resize(voxels * sizeof(int16_t));
        std::fseek(in, static_cast<long>(header.sdf_data_offset), SEEK_SET);
        if (std::fread(sdf_data.data(), 1, sdf_data.size(), in) != sdf_data.size()) {
            std::fclose(in);
            return false;
        }
    }
    std::fclose(in);

    auto* out_header = reinterpret_cast<FjMeshHeader*>(prefix.data());
    out_header->collision_vertex_count = static_cast<uint32_t>(collision.vertices.size());
    out_header->collision_index_count = static_cast<uint32_t>(collision.indices.size());
    // The full-detail counts carry over from the header already in `prefix`:
    // that geometry is preserved verbatim, only its offsets move.

    // Re-lay out everything from collision onward, matching the alignment the
    // importer uses so a rewritten file is laid out like a freshly built one.
    uint64_t offset = prefix_end;
    out_header->collision_vertices_offset = align_to(offset, 4);
    offset = out_header->collision_vertices_offset +
             (collision.vertices.size() * sizeof(glm::vec3));

    out_header->collision_indices_offset = align_to(offset, 4);
    offset = out_header->collision_indices_offset +
             (collision.indices.size() * sizeof(uint32_t));

    // When the new geometry is the full-detail geometry, both sections name the
    // same bytes, exactly as a fresh import writes it.
    const bool derived_is_full = collision.vertices == full_vertices &&
                                 collision.indices == full_indices;
    if (derived_is_full) {
        out_header->collision_full_vertices_offset = out_header->collision_vertices_offset;
        out_header->collision_full_indices_offset = out_header->collision_indices_offset;
    } else {
        out_header->collision_full_vertices_offset = align_to(offset, 4);
        offset = out_header->collision_full_vertices_offset +
                 (full_vertices.size() * sizeof(glm::vec3));

        out_header->collision_full_indices_offset = align_to(offset, 4);
        offset = out_header->collision_full_indices_offset +
                 (full_indices.size() * sizeof(uint32_t));
    }

    out_header->lod_levels_offset = align_to(offset, 4);
    offset = out_header->lod_levels_offset + lod_levels.size();

    if (!sdf_data.empty()) {
        out_header->sdf_data_offset = align_to(offset, 4);
        offset = out_header->sdf_data_offset + sdf_data.size();
    } else {
        out_header->sdf_data_offset = 0;
    }

    AuthoredLayout layout;
    layout_authored_sections(*out_header, offset, authored, layout);

    std::string tmp = path + ".tmp";
    std::FILE* out = std::fopen(tmp.c_str(), "wb");
    if (out == nullptr) {
        return false;
    }

    std::fwrite(prefix.data(), 1, prefix.size(), out);

    pad_to_offset(out, out_header->collision_vertices_offset);
    std::fwrite(collision.vertices.data(), sizeof(glm::vec3), collision.vertices.size(), out);

    pad_to_offset(out, out_header->collision_indices_offset);
    std::fwrite(collision.indices.data(), sizeof(uint32_t), collision.indices.size(), out);

    if (!derived_is_full) {
        pad_to_offset(out, out_header->collision_full_vertices_offset);
        std::fwrite(full_vertices.data(), sizeof(glm::vec3), full_vertices.size(), out);

        pad_to_offset(out, out_header->collision_full_indices_offset);
        std::fwrite(full_indices.data(), sizeof(uint32_t), full_indices.size(), out);
    }

    pad_to_offset(out, out_header->lod_levels_offset);
    std::fwrite(lod_levels.data(), 1, lod_levels.size(), out);

    if (!sdf_data.empty()) {
        pad_to_offset(out, out_header->sdf_data_offset);
        std::fwrite(sdf_data.data(), 1, sdf_data.size(), out);
    }

    write_authored_sections(out, *out_header, layout);

    // The header was only finalized during layout, so the copy written at the
    // top of the file is stale until it is put back.
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(out_header, sizeof(FjMeshHeader), 1, out);
    std::fclose(out);

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string find_import_source(const std::string& fjmesh_path) {
    namespace fs = std::filesystem;

    auto authored = read_authored_data(fjmesh_path);
    if (authored.source_file.empty()) return {};

    fs::path dir = fs::path(fjmesh_path).parent_path();
    std::error_code ec;

    // The recorded path is relative to the .fjmesh, so this resolves it
    // exactly wherever the pair has been moved to together.
    auto candidate = fs::weakly_canonical(dir / authored.source_file, ec);
    if (!ec && fs::exists(candidate, ec)) return candidate.string();

    // A mesh imported from an unrelated tree stores only a filename; a source
    // sitting beside it is then the best that can be inferred.
    auto beside = dir / fs::path(authored.source_file).filename();
    if (fs::exists(beside, ec)) return beside.string();

    return {};
}

void merge_authored_data(MeshAuthoredData& fresh, const MeshAuthoredData& previous) {
    if (previous.empty()) {
        return;
    }

    fresh.collision_source = previous.collision_source;
    fresh.collision_simplify_ratio = previous.collision_simplify_ratio;
    fresh.collision_mesh = previous.collision_mesh;
    // A rebuilt mesh is the same asset, so it keeps the same identity.
    if (!previous.asset_uuid.empty()) fresh.asset_uuid = previous.asset_uuid;

    std::unordered_map<std::string, std::string> assigned;
    for (const auto& slot : previous.material_slots) {
        if (!slot.path.empty()) {
            assigned.emplace(slot.name, slot.path);
        }
    }

    // Slots come from the new geometry; only the assignment carries over, and
    // only where the source material name still exists.
    for (auto& slot : fresh.material_slots) {
        auto it = assigned.find(slot.name);
        if (it != assigned.end()) {
            slot.path = it->second;
        }
    }
}

} // namespace fjell
