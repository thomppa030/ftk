#include "renderer/post_layers.hpp"

#include "scene/volume_occupancy.hpp"

#include <algorithm>

namespace fjell {

std::vector<PostLayer> plan_post_layers(const PostLayer& engine_default,
                                        std::span<const PostCandidate> volumes,
                                        const glm::vec3& camera) {
    struct Contribution {
        const PostCandidate* candidate;
        float weight;
    };
    std::vector<Contribution> contributions;
    contributions.reserve(volumes.size());
    for (const auto& candidate : volumes) {
        const PostVolume& volume = *candidate.volume;
        float weight = std::clamp(volume.weight, 0.0f, 1.0f);
        if (!volume.infinite) {
            weight *= volume_occupancy(volume.shape, glm::inverse(volume.transform), camera,
                                       volume.edge_falloff);
        }
        if (weight > 0.0f) contributions.push_back({&candidate, weight});
    }
    // Stable, so volumes of one priority apply in scene order and the frame
    // does not change with the sort's mood.
    std::stable_sort(contributions.begin(), contributions.end(),
                     [](const Contribution& a, const Contribution& b) {
                         return a.candidate->volume->priority < b.candidate->volume->priority;
                     });

    std::vector<PostLayer> layers{engine_default};
    layers.front().weight = 1.0f;
    for (const auto& [candidate, weight] : contributions) {
        const auto& values = candidate->volume->uniforms;
        PostLayer& top = layers.back();
        if (top.look.same_look(candidate->look)) {
            // One shader, one set of textures: the values blend, which is
            // one draw however many volumes the camera stands in.
            mix_named_values(top.values, values, weight);
            continue;
        }
        if (weight >= 1.0f) layers.clear();
        layers.push_back({candidate->look, values, layers.empty() ? 1.0f : weight});
    }
    return layers;
}

} // namespace fjell
