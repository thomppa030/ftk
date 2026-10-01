#include "ftk/gpu/device.hpp"

#include <format>

namespace ftk::gpu {

std::vector<std::string> missing_caps(const Caps& caps, const Caps& required) {
    std::vector<std::string> missing;
    if (required.mesh_shaders && !caps.mesh_shaders) {
        missing.emplace_back("mesh shaders (NVIDIA Turing, AMD RDNA 2, Intel Arc or newer)");
    }
    if (caps.mesh_max_output_vertices < required.mesh_max_output_vertices) {
        missing.push_back(std::format("mesh shaders that output {} vertices", required.mesh_max_output_vertices));
    }
    if (caps.mesh_max_output_primitives < required.mesh_max_output_primitives) {
        missing.push_back(std::format("mesh shaders that output {} primitives", required.mesh_max_output_primitives));
    }
    if (caps.max_push_size < required.max_push_size) {
        missing.push_back(std::format("{} bytes of push data", required.max_push_size));
    }
    if (required.async_compute && !caps.async_compute) {
        missing.emplace_back("a compute queue of its own");
    }
    if (caps.frames_in_flight < required.frames_in_flight) {
        missing.push_back(std::format("{} frames in flight", required.frames_in_flight));
    }
    if (caps.max_samples < required.max_samples) {
        missing.push_back(std::format("{}x multisampling", static_cast<int>(required.max_samples)));
    }
    if (required.ray_queries && !caps.ray_queries) {
        missing.emplace_back("ray queries");
    }
    return missing;
}

} // namespace ftk::gpu
