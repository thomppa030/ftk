#include "scene/removal_epoch.hpp"

#include <atomic>

namespace fjell {

namespace {
// Preview and thumbnail scenes are built off the main thread.
std::atomic<uint64_t> g_removal_epoch{1};
} // namespace

uint64_t removal_epoch() { return g_removal_epoch.load(std::memory_order_relaxed); }

void note_removal() { g_removal_epoch.fetch_add(1, std::memory_order_relaxed); }

const std::atomic<uint64_t>& removal_epoch_counter() { return g_removal_epoch; }

} // namespace fjell
