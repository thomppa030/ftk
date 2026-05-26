#pragma once

#include "renderer/resources/material.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace fjell {

class BindlessTextures;
class PipelineCache;

// Runtime-mutable clone of a parent Material. Owns its own row in the
// bindless material SSBO and its own slice of the custom-params pool, so
// changing a value here doesn't affect any other mesh sharing the parent.
//
// Created lazily by MeshComponent::set_param() — the parent is left alone
// for the common case where every instance of a material renders the same.
//
// Lifetime: held by unique_ptr on the MeshComponent (or wherever else the
// caller chooses). On destruction the SSBO row stays allocated (the
// bindless registry is append-only by design) but stops being referenced.
class MaterialInstance {
public:
    // Create a MID cloning `parent`. Allocates a fresh material slot + a
    // fresh custom-params range, seeded with the parent's current values.
    // `pipeline_cache` is used to look up uniform names → slots for
    // set_param().
    MaterialInstance(const Material& parent,
                     BindlessTextures& bindless,
                     PipelineCache& pipeline_cache);

    // Param mutation by uniform name. Type must match the shader
    // declaration; mismatched calls log a warning and no-op.
    void set_param(const std::string& name, float value);
    void set_param(const std::string& name, const glm::vec2& value);
    void set_param(const std::string& name, const glm::vec3& value);
    void set_param(const std::string& name, const glm::vec4& value);

    [[nodiscard]] uint32_t material_id() const { return material_.material_id(); }
    [[nodiscard]] const Material& material() const { return material_; }
    [[nodiscard]] const Material* parent() const { return parent_; }

private:
    void write_floats(const std::string& name, const float* data, uint32_t count);

    const Material* parent_;
    Material material_;            // value copy with its own material_id + params offset
    BindlessTextures* bindless_;
    PipelineCache* pipeline_cache_;
};

} // namespace fjell
