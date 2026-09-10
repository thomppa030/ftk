#include "renderer/resources/fjmesh_writer.hpp"

#include <cstddef>
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

void merge_authored_data(MeshAuthoredData& fresh, const MeshAuthoredData& previous) {
    if (previous.empty()) {
        return;
    }

    fresh.collision_source = previous.collision_source;
    fresh.collision_simplify_ratio = previous.collision_simplify_ratio;
    fresh.collision_mesh = previous.collision_mesh;

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
