#include "ftk/base/range_allocator.hpp"

#include <cassert>
#include <iterator>

namespace ftk {

uint64_t RangeAllocator::allocate(uint64_t size) {
    if (size == 0) return end_;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const auto [offset, gap] = *it;
        if (gap < size) continue;
        free_.erase(it);
        if (gap > size) free_.emplace(offset + size, gap - size);
        free_units_ -= size;
        return offset;
    }
    const uint64_t offset = end_;
    end_ += size;
    return offset;
}

void RangeAllocator::free(uint64_t offset, uint64_t size) {
    if (size == 0) return;
    assert(offset + size <= end_ && "RangeAllocator: a range past the end");

    uint64_t start = offset;
    uint64_t length = size;
    // Join the gap that ends where this starts, and the one that starts where
    // this ends.
    auto next = free_.lower_bound(offset);
    assert((next == free_.end() || next->first >= offset + size) && "RangeAllocator: a range freed twice");
    if (next != free_.begin()) {
        const auto before = std::prev(next);
        assert(before->first + before->second <= offset && "RangeAllocator: a range freed twice");
        if (before->first + before->second == offset) {
            start = before->first;
            length += before->second;
            free_units_ -= before->second;
            free_.erase(before);
        }
    }
    if (next != free_.end() && next->first == offset + size) {
        length += next->second;
        free_units_ -= next->second;
        free_.erase(next);
    }

    // A gap reaching the end is no gap: what is in use ends before it.
    if (start + length == end_) {
        end_ = start;
        return;
    }
    free_.emplace(start, length);
    free_units_ += length;
}

void RangeAllocator::clear() {
    free_.clear();
    end_ = 0;
    free_units_ = 0;
}

} // namespace ftk
