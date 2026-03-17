#pragma once

#include <string>
#include <string_view>

namespace fjell {

class Layer {
public:
    explicit Layer(std::string_view name) : name_{name} {}
    virtual ~Layer() = default;

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
    Layer(Layer&&) = delete;
    Layer& operator=(Layer&&) = delete;

    virtual void on_update(float /*dt*/) {}
    virtual void on_imgui() {}
    [[nodiscard]] virtual bool wants_input() const { return false; }

    [[nodiscard]] const std::string& name() const { return name_; }

private:
    std::string name_;
};

} // namespace fjell
