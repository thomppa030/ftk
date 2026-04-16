#pragma once

#include "scripting/fjell/weak_ref.hpp"

#include <cstdint>
#include <unordered_set>
#include <span>

namespace fjell {

/// Walks reflected reference fields during GC sweeps.
/// Engine creates one, sets doomed IDs, then calls generated trace functions
/// which invoke the extern "C" bridge functions below.
class GcVisitor {
public:
    void set_doomed_ids(std::span<const uint32_t> ids) {
        doomed_.clear();
        doomed_.insert(ids.begin(), ids.end());
    }

    /// Null a WeakRef if its target is in the doomed set.
    void trace_weak(WeakRefData& ref) {
        if (ref.id != 0 && doomed_.contains(ref.id)) {
            ref = {};
        }
    }

private:
    std::unordered_set<uint32_t> doomed_;
};

} // namespace fjell
