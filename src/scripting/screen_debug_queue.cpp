// screen_debug_queue.cpp — Script-submitted screen-space debug text
#include "scripting/screen_debug_queue.hpp"

#include <algorithm>

namespace fjell {

void ScreenDebugQueue::message(int key, const std::string& text,
                               const glm::vec4& color, float ttl) {
    if (key >= 0) {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [key](const ScreenDebugEntry& e) {
                                   return e.kind == ScreenDebugEntry::Kind::Message &&
                                          e.key == key;
                               });
        if (it != entries_.end()) {
            // Keep the original seq so a keyed line holds its slot in the
            // block instead of jumping to the bottom on every refresh.
            it->text = text;
            it->color = color;
            it->ttl = ttl;
            return;
        }
    }

    ScreenDebugEntry entry;
    entry.kind = ScreenDebugEntry::Kind::Message;
    entry.text = text;
    entry.key = key;
    entry.color = color;
    entry.ttl = ttl;
    entry.seq = next_seq_++;
    entries_.push_back(std::move(entry));
}

void ScreenDebugQueue::watch(const std::string& name, const std::string& value,
                             const glm::vec4& color) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&name](const ScreenDebugEntry& e) {
                               return e.kind == ScreenDebugEntry::Kind::Watch &&
                                      e.name == name;
                           });
    if (it != entries_.end()) {
        it->text = value;
        it->color = color;
        it->ttl = 0.0f;
        return;
    }

    ScreenDebugEntry entry;
    entry.kind = ScreenDebugEntry::Kind::Watch;
    entry.name = name;
    entry.text = value;
    entry.color = color;
    entry.ttl = 0.0f;
    entry.seq = next_seq_++;
    entries_.push_back(std::move(entry));
}

void ScreenDebugQueue::world_text(const glm::vec3& world_pos, const std::string& text,
                                  const glm::vec4& color, float ttl) {
    ScreenDebugEntry entry;
    entry.kind = ScreenDebugEntry::Kind::World;
    entry.text = text;
    entry.world_pos = world_pos;
    entry.color = color;
    entry.ttl = ttl;
    entry.seq = next_seq_++;
    entries_.push_back(std::move(entry));
}

void ScreenDebugQueue::tick(float dt) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->ttl <= 0.0f) {
            // Single-frame entry — it has been drawn once by now, so drop it.
            // A watch re-submitted every tick reappears immediately; one whose
            // script stopped writing it disappears, which is what you want.
            it = entries_.erase(it);
        } else {
            it->ttl -= dt;
            ++it;
        }
    }
}

void ScreenDebugQueue::clear() {
    entries_.clear();
}

} // namespace fjell
