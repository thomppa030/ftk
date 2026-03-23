#pragma once

#include <cstdint>
#include <string_view>

namespace fjell {

struct TickContext;

enum class TickPolicy : uint8_t {
    ALWAYS,
    PLAY_ONLY,
};

class Subsystem {
public:
    virtual ~Subsystem() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual TickPolicy tick_policy() const { return TickPolicy::ALWAYS; }

    virtual void tick(const TickContext& /*ctx*/) {}
    virtual void on_play_start(const TickContext& /*ctx*/) {}
    virtual void on_play_stop(const TickContext& /*ctx*/) {}

    Subsystem() = default;
    Subsystem(const Subsystem&) = delete;
    Subsystem& operator=(const Subsystem&) = delete;
    Subsystem(Subsystem&&) = delete;
    Subsystem& operator=(Subsystem&&) = delete;
};

} // namespace fjell
