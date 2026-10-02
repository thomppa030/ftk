#pragma once

#include <cstdint>
#include <map>

namespace ftk {

/// Places ranges in something that only has a length: a shared buffer's
/// vertices, a table's rows. A range given back is reused by the next one
/// that fits in it, first by position, and joins the free ranges either side
/// of it; one at the end shortens what is in use instead. Units are the
/// caller's (elements, rows, floats), never bytes unless it says so.
///
/// It knows nothing of the GPU: whoever gives a range back does it once
/// nothing reads that range any more.
///
/// @code
/// RangeAllocator vertices;
/// const uint64_t at = vertices.allocate(mesh.vertex_count);
/// buffer.grow_to(vertices.end());
/// ...
/// vertices.free(at, mesh.vertex_count);
/// @endcode
class RangeAllocator {
public:
    /// Where `size` units go: the start of the first free range they fit,
    /// or the end, which moves past them. Nothing for a size of 0.
    [[nodiscard]] uint64_t allocate(uint64_t size);

    /// Gives back `size` units at `offset`, which `allocate()` handed out and
    /// nothing has given back since.
    void free(uint64_t offset, uint64_t size);

    /// Where the ranges in use end: what the buffer behind them must hold.
    [[nodiscard]] uint64_t end() const { return end_; }
    /// Units free below `end()`, in gaps between ranges in use.
    [[nodiscard]] uint64_t free_units() const { return free_units_; }
    /// Units in ranges in use.
    [[nodiscard]] uint64_t used() const { return end_ - free_units_; }

    /// Forgets every range: everything is free again.
    void clear();

private:
    // Free gaps below end_, by offset, with their sizes; neighbours never
    // touch, since freeing joins them.
    std::map<uint64_t, uint64_t> free_;
    uint64_t end_{0};
    uint64_t free_units_{0};
};

} // namespace ftk
