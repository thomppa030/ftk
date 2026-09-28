#pragma once

#include "renderer/resources/resource_handle.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

/// The resources of one kind, by generational handle and by the key (a path,
/// or a name for what was made in code) they were loaded under.
///
/// A slot is Loading while an asynchronous load fills it, Ready once it holds
/// its resource, and Failed when its load failed. A failed load keeps its slot:
/// the key stays known, so whatever holds the handle can say which file it
/// meant and save it back, and the next load of that key tries again into the
/// same slot, which brings every holder back with it.
template <typename T, typename HandleT>
struct ResourcePool {
    std::vector<std::unique_ptr<T>> slots;
    std::vector<uint32_t> generations;
    std::vector<LoadState> states;
    std::unordered_map<std::string, HandleT> name_to_handle;

    /// A new Ready slot holding `resource`, found by `key` unless it is empty.
    HandleT insert(const std::string& key, std::unique_ptr<T> resource) {
        const HandleT handle = add_slot(key, LoadState::Ready);
        slots[handle.index()] = std::move(resource);
        return handle;
    }

    /// The resource, or null for a slot that is loading, failed, or not this
    /// pool's.
    [[nodiscard]] T* get(HandleT handle) const {
        if (!owns(handle)) return nullptr;
        return slots[handle.index()].get();
    }

    /// A Loading slot for `key`, for an asynchronous load to fill.
    HandleT reserve(const std::string& key) { return add_slot(key, LoadState::Loading); }

    /// Fills a reserved or failed slot; it is Ready from now on.
    void emplace(HandleT handle, std::unique_ptr<T> resource) {
        if (!owns(handle)) return;
        slots[handle.index()] = std::move(resource);
        states[handle.index()] = LoadState::Ready;
    }

    /// The slot's state; Failed for a handle that is not this pool's.
    [[nodiscard]] LoadState state(HandleT handle) const {
        return owns(handle) ? states[handle.index()] : LoadState::Failed;
    }

    /// The handle kept for `key` in any state, or an invalid one when nothing
    /// was ever loaded under it.
    [[nodiscard]] HandleT find(const std::string& key) const {
        const auto it = name_to_handle.find(key);
        return it != name_to_handle.end() ? it->second : HandleT{};
    }

    /// The handle for `key` once its resource is there, or an invalid one
    /// while it is loading, after it failed, or when it was never asked for.
    [[nodiscard]] HandleT ready(const std::string& key) const {
        const HandleT handle = find(key);
        return state(handle) == LoadState::Ready ? handle : HandleT{};
    }

    /// Records that loading `key` failed: its slot, a new one if it had none,
    /// holds nothing and reads Failed.
    HandleT fail(const std::string& key) {
        HandleT handle = find(key);
        if (!handle.valid()) handle = add_slot(key, LoadState::Failed);
        slots[handle.index()].reset();
        states[handle.index()] = LoadState::Failed;
        return handle;
    }

    /// Puts `resource` under `key`: into the slot an earlier attempt left, so
    /// the handles given out for it find it, or into a new one.
    HandleT store(const std::string& key, std::unique_ptr<T> resource) {
        const HandleT handle = find(key);
        if (!handle.valid()) return insert(key, std::move(resource));
        emplace(handle, std::move(resource));
        return handle;
    }

    /// The key a handle was loaded under, or empty.
    [[nodiscard]] std::string path_of(HandleT handle) const {
        for (const auto& [key, value] : name_to_handle) {
            if (value == handle) return key;
        }
        return {};
    }

private:
    [[nodiscard]] bool owns(HandleT handle) const {
        if (!handle.valid()) return false;
        const uint32_t index = handle.index();
        return index < slots.size() && generations[index] == handle.generation();
    }

    HandleT add_slot(const std::string& key, LoadState state) {
        const auto index = static_cast<uint32_t>(slots.size());
        const uint32_t generation = 1;
        slots.emplace_back();
        generations.push_back(generation);
        states.push_back(state);
        const HandleT handle = HandleT::make(index, generation);
        if (!key.empty()) name_to_handle.emplace(key, handle);
        return handle;
    }
};

} // namespace fjell
