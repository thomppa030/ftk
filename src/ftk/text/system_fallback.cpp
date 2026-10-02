#include "ftk/text/system_fallback.hpp"

#include "ftk/base/log.hpp"

namespace ftk::text {

SystemFallback::SystemFallback(std::string language, Lookup lookup)
    : language_{std::move(language)}, lookup_{std::move(lookup)} {}

const Face* SystemFallback::face_for(char32_t codepoint) {
    if (auto known = by_codepoint_.find(codepoint); known != by_codepoint_.end()) return known->second;

    const Face* face = nullptr;
    if (const std::optional<platform::FontFile> file = lookup_ ? lookup_(codepoint, language_) : std::nullopt) {
        auto key = std::make_pair(file->path, file->index);
        auto loaded = faces_.find(key);
        if (loaded == faces_.end()) {
            auto read = Face::load(file->path, file->index);
            if (!read) {
                FTK_CORE_WARN("Text: the system's fallback font can't be read: {}", read.error());
            }
            // Remembered either way, so a broken file is read once.
            loaded = faces_.emplace(std::move(key), read ? *read : nullptr).first;
        }
        // A lookup is trusted only as far as the face it names draws the
        // character.
        if (loaded->second != nullptr && loaded->second->covers(codepoint)) face = loaded->second.get();
    }
    by_codepoint_.emplace(codepoint, face);
    return face;
}

} // namespace ftk::text
