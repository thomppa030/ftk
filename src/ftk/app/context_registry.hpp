#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace ftk {

/// How one kind of asset opens. An asset has a tab of its own: `matches` says
/// whether a context already shows `path`, and `open` then points that
/// context at it without reloading (a clip in its skeleton's tab); only when
/// none matches is a new context made with `create`. A kind whose tab can
/// hold several, as textures can, opens a set of them together: `holds_set`
/// says whether a context holds exactly `paths`, and `create_set` makes one
/// that does. `Context` is the host's own kind of context, so what it creates
/// needs no cast.
template <class Context>
struct ContextFactoryEntry {
    std::function<std::unique_ptr<Context>(const std::string& path)> create;
    std::function<void(Context& ctx, const std::string& path)> open;
    std::function<bool(const Context& ctx, const std::string& path)> matches;
    std::function<std::unique_ptr<Context>(std::span<const std::string> paths)> create_set{};
    std::function<bool(const Context& ctx, std::span<const std::string> paths)> holds_set{};
};

template <class Context>
class ContextRegistry {
public:
    void register_type(const std::string& asset_type, ContextFactoryEntry<Context> entry) {
        factories_[asset_type] = std::move(entry);
    }

    [[nodiscard]] const ContextFactoryEntry<Context>* find(const std::string& asset_type) const {
        auto it = factories_.find(asset_type);
        return it != factories_.end() ? &it->second : nullptr;
    }

private:
    std::unordered_map<std::string, ContextFactoryEntry<Context>> factories_;
};

} // namespace ftk
