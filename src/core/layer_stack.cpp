#include "core/layer_stack.hpp"

namespace fjell {

void LayerStack::push_layer(std::unique_ptr<Layer> layer) {
    layers_.insert(layers_.begin() + static_cast<ptrdiff_t>(overlay_start_),
                   std::move(layer));
    ++overlay_start_;
}

void LayerStack::push_overlay(std::unique_ptr<Layer> overlay) {
    layers_.push_back(std::move(overlay));
}

void LayerStack::update_all(float dt) {
    for (auto& layer : layers_) {
        layer->on_update(dt);
    }
}

void LayerStack::imgui_all() {
    for (auto& layer : layers_) {
        layer->on_imgui();
    }
}

std::string_view LayerStack::input_owner() const {
    // Search overlays first (end → overlay_start), then layers (overlay_start → begin)
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if ((*it)->wants_input()) {
            return (*it)->name();
        }
    }
    return {};
}

bool LayerStack::has_input(std::string_view name) const {
    return input_owner() == name;
}

} // namespace fjell
