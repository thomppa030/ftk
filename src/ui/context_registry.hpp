#pragma once

#include "ui/editor_context.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

/// How one kind of asset opens. Every asset has its own tab: `matches` says
/// whether a context already shows `path`, and `open` then points that
/// context at it without reloading (a clip in its skeleton's tab); only when
/// none matches is a new context made with `create`.
struct ContextFactoryEntry {
    std::function<std::unique_ptr<EditorContext>(const std::string& path)> create;
    std::function<void(EditorContext& ctx, const std::string& path)> open;
    std::function<bool(const EditorContext& ctx, const std::string& path)> matches;
};

class ContextRegistry {
public:
    void register_type(const std::string& asset_type, ContextFactoryEntry entry);
    [[nodiscard]] const ContextFactoryEntry* find(const std::string& asset_type) const;

private:
    std::unordered_map<std::string, ContextFactoryEntry> factories_;
};

} // namespace fjell
