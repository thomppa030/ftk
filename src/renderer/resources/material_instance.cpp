#include "renderer/resources/material_instance.hpp"

#include "ftk/base/log.hpp"
#include "renderer/bindless.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/fjsl_parser.hpp"

#include <algorithm>
#include <array>

namespace fjell {

namespace {

// Snapshot the parent's current custom-params slice. Reading float-by-float
// keeps the API of BindlessTextures small (we don't need to expose the
// underlying vector).
std::vector<float> snapshot_params(const BindlessTextures& b,
                                   uint32_t offset, uint32_t count) {
    std::vector<float> out(count);
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = b.read_param(offset + i);
    }
    return out;
}

} // namespace

MaterialInstance::MaterialInstance(const Material& parent,
                                   BindlessTextures& bindless,
                                   PipelineCache& pipeline_cache)
    : parent_{&parent}
    , material_{parent}              // copy parent wholesale (textures, PbrParams, paths, flags)
    , bindless_{&bindless}
    , pipeline_cache_{&pipeline_cache} {
    // Allocate a fresh custom-params slice and seed it with the parent's values.
    const uint32_t count = parent.custom_param_count();
    uint32_t new_offset = 0;
    if (count > 0) {
        new_offset = bindless.allocate_params(count);
        auto seed = snapshot_params(bindless, parent.custom_param_offset(), count);
        bindless.write_params(new_offset, seed.data(), count);
    }
    material_.set_custom_params(new_offset, count);

    // Allocate a fresh row in the material SSBO; the row references the new
    // params slice via material_.gpu_material().
    material_.material_id_ = bindless.add_material(material_.gpu_material());
}

void MaterialInstance::set_param(const std::string& name, float v) {
    write_floats(name, &v, 1);
}
void MaterialInstance::set_param(const std::string& name, const glm::vec2& v) {
    std::array data{v.x, v.y};
    write_floats(name, data.data(), 2);
}
void MaterialInstance::set_param(const std::string& name, const glm::vec3& v) {
    std::array data{v.x, v.y, v.z};
    write_floats(name, data.data(), 3);
}
void MaterialInstance::set_param(const std::string& name, const glm::vec4& v) {
    std::array data{v.x, v.y, v.z, v.w};
    write_floats(name, data.data(), 4);
}

void MaterialInstance::write_floats(const std::string& name,
                                    const float* data, uint32_t count) {
    const auto& meta = pipeline_cache_->metadata(material_.pipeline_id());
    for (const auto& u : meta.uniforms) {
        if (u.name != name) continue;
        if (u.type == UniformType::Sampler2D) {
            FTK_GFX_WARN("MaterialInstance::set_param('{}'): uniform is a sampler, "
                           "not a scalar/vector — ignored", name);
            return;
        }
        const uint32_t expected = uniform_float_count(u.type);
        if (expected != count) {
            FTK_GFX_WARN("MaterialInstance::set_param('{}'): type mismatch "
                           "(shader expects {} floats, got {}) — ignored",
                           name, expected, count);
            return;
        }
        std::array<float, 4> encoded{};
        std::copy_n(data, count, encoded.begin());
        encode_uniform_for_gpu(u, std::span{encoded}.first(count));
        bindless_->write_params(material_.custom_param_offset() + u.slot,
                                encoded.data(), count);
        return;
    }
    FTK_GFX_WARN("MaterialInstance::set_param('{}'): no such uniform on shader '{}'",
                   name, material_.shader_path());
}

} // namespace fjell
