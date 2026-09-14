// What a material emits, for the paths that hit its surface from a ray or
// shade a particle with it and cannot run the material's fragment().
//
// Callers sample texture_indices[3] themselves, however their stage samples
// (textureLod in a hit shader, texture in a fragment shader), and pass the
// result in. That slot always holds a valid texture: the material's own, or
// the shared black default when it bound none. Which of those it is decides
// how the sample is used, and only the CPU knows, so it says so through
// has_emissive_texture:
//
//   no texture       -> the emission colour alone
//   texture bound    -> emission colour * sample, everywhere, so a black
//                       texel means no emission there rather than "no
//                       texture, keep the flat colour"
//
// Requires GpuMaterial from fjell_gpu_types.glsl.
vec3 material_emission(GpuMaterial mat, vec3 emissive_sample) {
    vec3 emission = vec3(mat.base_params[9], mat.base_params[10], mat.base_params[11]);
    if (mat.has_emissive_texture != 0u) {
        emission *= emissive_sample;
    }
    return emission;
}
