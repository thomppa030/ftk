#include "gpu/vulkan/command_list_impl.hpp"

#include <functional>
#include <string>

// Zones: a debug label around a span of a list's commands and, in profiling
// builds, a GPU zone the profiler times.

namespace fjell::gpu {

#ifdef FJELL_ENABLE_TRACY

size_t Device::Impl::ZoneSiteHash::operator()(const ZoneSiteKey& key) const noexcept {
    const size_t name = std::hash<std::string_view>{}(key.name);
    const size_t file = std::hash<const void*>{}(key.file);
    return name ^ (file << 1) ^ (static_cast<size_t>(key.line) << 2);
}

const tracy::SourceLocationData* Device::Impl::zone_source(std::string_view zone_name,
                                                           const std::source_location& where) {
    const ZoneSiteKey key{zone_name, where.file_name(), where.line()};
    std::lock_guard lock(zone_sites_mutex);
    auto found = zone_sites.find(key);
    if (found == zone_sites.end()) {
        found = zone_sites.emplace(ZoneSite{std::string(zone_name), key.file, key.line},
                                   tracy::SourceLocationData{})
                    .first;
        // The map's nodes never move, so the profiler may keep pointing at
        // the name and at the record.
        found->second = {found->first.name.c_str(), where.function_name(), key.file, key.line, 0};
    }
    return &found->second;
}

#endif

Zone CommandList::zone(std::string_view name, std::source_location where) {
    Impl& self = *impl_;
    Device::Impl& device = device_->impl();
    if (device.begin_label != nullptr) {
        const std::string label(name);
        VkDebugUtilsLabelEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        info.pLabelName = label.c_str();
        device.begin_label(self.cb, &info);
    }
#ifdef FJELL_ENABLE_TRACY
    if (device.profiler != nullptr && self.zone_depth < Impl::MAX_ZONE_DEPTH) {
        self.zones[self.zone_depth].emplace(device.profiler, device.zone_source(name, where),
                                            self.cb, true);
    }
#else
    (void)where;
#endif
    ++self.zone_depth;
    return Zone(*this);
}

void CommandList::end_zone() {
    Impl& self = *impl_;
    --self.zone_depth;
#ifdef FJELL_ENABLE_TRACY
    if (self.zone_depth < Impl::MAX_ZONE_DEPTH) self.zones[self.zone_depth].reset();
#endif
    if (device_->impl().end_label != nullptr) device_->impl().end_label(self.cb);
}

Zone::~Zone() { list_->end_zone(); }

} // namespace fjell::gpu
