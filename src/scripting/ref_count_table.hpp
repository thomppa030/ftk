#pragma once

#include <cstdint>
#include <vector>

namespace fjell {

/// Engine-side token-based reference counting.
/// Scripts hold tokens (indices into this table). The engine manages the actual counts.
class RefCountTable {
public:
    /// Allocate a new token with refcount 1, associated with a resource handle.
    [[nodiscard]] uint32_t create(uint32_t handle_id) {
        if (!free_list_.empty()) {
            uint32_t token = free_list_.back();
            free_list_.pop_back();
            entries_[token] = {handle_id, 1};
            return token;
        }
        uint32_t token = static_cast<uint32_t>(entries_.size());
        entries_.push_back({handle_id, 1});
        return token;
    }

    void acquire(uint32_t token) {
        if (token < entries_.size()) {
            entries_[token].count++;
        }
    }

    /// Returns true if the refcount hit zero (caller should clean up the resource).
    bool release(uint32_t token) {
        if (token >= entries_.size()) return false;
        auto& e = entries_[token];
        if (e.count == 0) return false;
        e.count--;
        if (e.count == 0) {
            free_list_.push_back(token);
            return true;
        }
        return false;
    }

    [[nodiscard]] uint32_t handle_id(uint32_t token) const {
        if (token >= entries_.size()) return 0;
        return entries_[token].handle_id;
    }

    [[nodiscard]] uint32_t count(uint32_t token) const {
        if (token >= entries_.size()) return 0;
        return entries_[token].count;
    }

    void clear() {
        entries_.clear();
        free_list_.clear();
    }

private:
    struct Entry {
        uint32_t handle_id{0};
        uint32_t count{0};
    };

    std::vector<Entry> entries_;
    std::vector<uint32_t> free_list_;
};

} // namespace fjell
