#pragma once

#include "core/layer.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace fjell {

class LayerStack {
public:
    void push_layer(std::unique_ptr<Layer> layer);
    void push_overlay(std::unique_ptr<Layer> overlay);

    void update_all(float dt);
    void imgui_all();

    [[nodiscard]] std::string_view input_owner() const;
    [[nodiscard]] bool has_input(std::string_view name) const;

    // Iteration
    [[nodiscard]] auto begin() { return layers_.begin(); }
    [[nodiscard]] auto end() { return layers_.end(); }
    [[nodiscard]] auto begin() const { return layers_.begin(); }
    [[nodiscard]] auto end() const { return layers_.end(); }

private:
    std::vector<std::unique_ptr<Layer>> layers_;
    std::size_t overlay_start_{0}; // index where overlays begin
};

} // namespace fjell
