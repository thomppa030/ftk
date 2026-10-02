#pragma once

// The system's fonts for what a chain's own faces lack, such as ideographs
// in a Latin UI font: the system is asked once per character, and each font
// file it names is loaded once.
//
//     auto system = std::make_shared<text::SystemFallback>("ja");
//     text::FontChain chain({inter, icons}, system);
//
// Asked from one thread at a time.

#include "ftk/platform/system_fonts.hpp"
#include "ftk/text/face.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ftk::text {

class SystemFallback {
public:
    /// How the system is asked: platform::fallback_font, or a stand-in.
    using Lookup = std::function<std::optional<platform::FontFile>(char32_t codepoint, std::string_view language)>;

    /// For text in `language`, a BCP 47 tag such as "ja" (empty when
    /// unknown), asking `lookup`.
    explicit SystemFallback(std::string language = {}, Lookup lookup = platform::fallback_font);

    /// The face the system picks to draw `codepoint`, or null when it has
    /// none that does.
    [[nodiscard]] const Face* face_for(char32_t codepoint);

private:
    std::string language_;
    Lookup lookup_;
    std::unordered_map<char32_t, const Face*> by_codepoint_;
    std::map<std::pair<std::filesystem::path, uint32_t>, std::shared_ptr<const Face>> faces_;
};

} // namespace ftk::text
