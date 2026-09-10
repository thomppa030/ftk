// screen_debug_queue.hpp — Script-submitted screen-space debug text
// CPU-side queue consumed by the play overlay each frame. No Vulkan dependency.
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

/// A line of debug text destined for the on-screen overlay.
struct ScreenDebugEntry {
    enum class Kind : uint8_t {
        Message,  // keyed or keyless line in the message block
        Watch,    // "name: value" row, sorted by name
        World,    // anchored to a world position, projected each frame
    };

    Kind kind{Kind::Message};
    std::string text;              // Message/World: the line; Watch: the value
    std::string name;              // Watch only: the label, also the identity key
    int key{-1};                   // Message only: >= 0 overwrites in place, -1 appends
    glm::vec3 world_pos{0.0f};     // World only
    glm::vec4 color{1.0f};
    float ttl{0.0f};               // seconds (0 = single frame)
    uint64_t seq{0};               // submission order, keeps keyless output stable
};

class ScreenDebugQueue {
public:
    /// Keyed messages replace the entry with the same key rather than stacking,
    /// so a call from tick() shows one steady line instead of a flood. A key
    /// below zero appends instead, which is what print() wants.
    void message(int key, const std::string& text, const glm::vec4& color, float ttl = 0.0f);

    /// Watches are identified by name, so re-submitting a name updates it in
    /// place. Rendered as a separate sorted block so rows don't jump around
    /// between frames as unrelated messages come and go.
    void watch(const std::string& name, const std::string& value, const glm::vec4& color);

    void world_text(const glm::vec3& world_pos, const std::string& text,
                    const glm::vec4& color, float ttl = 0.0f);

    /// Age out expired entries. Called once per frame after the overlay has
    /// drawn, so single-frame entries survive exactly the frame they were
    /// submitted in.
    void tick(float dt);

    [[nodiscard]] const std::vector<ScreenDebugEntry>& entries() const { return entries_; }

    void clear();

private:
    std::vector<ScreenDebugEntry> entries_;
    uint64_t next_seq_{0};
};

} // namespace fjell
