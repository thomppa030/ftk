#include "ftk/gpu/shader.hpp"

#include <spirv_cross.hpp>

#include <algorithm>
#include <exception>
#include <tuple>

namespace fjell::gpu {

namespace {

std::string binding_text(const ShaderBinding& b) {
    return "set " + std::to_string(b.set) + " binding " + std::to_string(b.binding) + " ('" +
           b.name + "')";
}

void sort_bindings(std::vector<ShaderBinding>& bindings) {
    std::ranges::sort(bindings, [](const ShaderBinding& l, const ShaderBinding& r) {
        return std::tie(l.set, l.binding) < std::tie(r.set, r.binding);
    });
}

} // namespace

const ShaderBinding* ShaderLayout::find(std::string_view name) const {
    for (const auto& binding : bindings) {
        if (binding.name == name) return &binding;
    }
    return nullptr;
}

Result<ShaderLayout> reflect(std::span<const uint32_t> spirv) {
    try {
        const spirv_cross::Compiler compiler(spirv.data(), spirv.size());
        const auto entry_points = compiler.get_entry_points_and_stages();
        if (entry_points.empty()) return make_error("Shader has no entry point");

        ShaderStage stage = ShaderStage::compute;
        switch (entry_points.front().execution_model) {
            case spv::ExecutionModelVertex:    stage = ShaderStage::vertex; break;
            case spv::ExecutionModelFragment:  stage = ShaderStage::fragment; break;
            case spv::ExecutionModelGLCompute: stage = ShaderStage::compute; break;
            case spv::ExecutionModelTaskEXT:   stage = ShaderStage::task; break;
            case spv::ExecutionModelMeshEXT:   stage = ShaderStage::mesh; break;
            default:
                return make_error("Shader stage is not one the GPU interface runs "
                                  "(ray tracing stages trace from compute instead)");
        }

        ShaderLayout layout;
        layout.stages = stage;
        const auto resources = compiler.get_shader_resources();
        auto add = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, BindingKind kind,
                       bool block) {
            for (const auto& resource : list) {
                ShaderBinding binding;
                const std::string& instance = compiler.get_name(resource.id);
                binding.block = block ? compiler.get_name(resource.base_type_id) : std::string{};
                binding.name = instance.empty() ? (block ? binding.block : resource.name) : instance;
                binding.set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                binding.kind = kind;
                const auto& type = compiler.get_type(resource.type_id);
                binding.array = !type.array.empty();
                binding.count = binding.array ? type.array.front() : 1;
                binding.stages = stage;
                layout.bindings.push_back(std::move(binding));
            }
        };
        add(resources.uniform_buffers, BindingKind::uniform_buffer, true);
        add(resources.storage_buffers, BindingKind::storage_buffer, true);
        add(resources.sampled_images, BindingKind::sampled_texture, false);
        add(resources.separate_images, BindingKind::texture, false);
        add(resources.separate_samplers, BindingKind::sampler, false);
        add(resources.storage_images, BindingKind::storage_texture, false);
        add(resources.acceleration_structures, BindingKind::acceleration_structure, false);
        sort_bindings(layout.bindings);

        for (const auto& push : resources.push_constant_buffers) {
            const auto& type = compiler.get_type(push.base_type_id);
            layout.push_size = std::max(layout.push_size,
                                        static_cast<uint32_t>(compiler.get_declared_struct_size(type)));
            layout.push_stages = stage;
        }

        if (stage == ShaderStage::compute || stage == ShaderStage::task || stage == ShaderStage::mesh) {
            auto& size = layout.workgroup_sizes[static_cast<size_t>(stage)];
            for (uint32_t axis = 0; axis < 3; ++axis) {
                size[axis] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
            }
        }
        if (stage == ShaderStage::mesh) {
            layout.mesh_max_vertices = compiler.get_execution_mode_argument(spv::ExecutionModeOutputVertices);
            layout.mesh_max_primitives =
                compiler.get_execution_mode_argument(spv::ExecutionModeOutputPrimitivesEXT);
        }
        return layout;
    } catch (const std::exception& error) {
        return make_error(std::string("Shader could not be read: ") + error.what());
    }
}

Result<ShaderLayout> merge(const ShaderLayout& a, const ShaderLayout& b) {
    ShaderLayout out = a;
    out.stages |= b.stages;
    for (const auto& incoming : b.bindings) {
        auto same_slot = std::ranges::find_if(out.bindings, [&](const ShaderBinding& known) {
            return known.set == incoming.set && known.binding == incoming.binding;
        });
        if (same_slot == out.bindings.end()) {
            out.bindings.push_back(incoming);
            continue;
        }
        if (same_slot->name != incoming.name || same_slot->kind != incoming.kind ||
            same_slot->array != incoming.array ||
            (!same_slot->array && same_slot->count != incoming.count)) {
            return make_error("Two stages declare " + binding_text(*same_slot) + " and " +
                              binding_text(incoming) + " differently");
        }
        // Each stage sizes an unsized array by the indices it uses, so two
        // stages may disagree on its length: the pipeline takes the longer,
        // and one sized at run time over either.
        if (same_slot->array && same_slot->count != 0) {
            same_slot->count = incoming.count == 0 ? 0 : std::max(same_slot->count, incoming.count);
        }
        same_slot->stages |= incoming.stages;
    }
    sort_bindings(out.bindings);

    out.push_size = std::max(a.push_size, b.push_size);
    out.push_stages |= b.push_stages;
    for (size_t stage = 0; stage < out.workgroup_sizes.size(); ++stage) {
        if (b.workgroup_sizes[stage][0] != 0) out.workgroup_sizes[stage] = b.workgroup_sizes[stage];
    }
    if (b.mesh_max_vertices != 0) {
        out.mesh_max_vertices = b.mesh_max_vertices;
        out.mesh_max_primitives = b.mesh_max_primitives;
    }
    return out;
}

} // namespace fjell::gpu
