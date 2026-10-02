#pragma once

// The system's own fallback fonts: asked which installed font draws a
// character the program's fonts lack, the system answers with a file.
// fontconfig on Linux (loaded when the system has it), DirectWrite on
// Windows.
//
//     if (auto file = platform::fallback_font(U'日', "ja")) load(file->path, file->index);

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace ftk::platform {

/// A font file and the face in it that is meant.
struct FontFile {
    std::filesystem::path path;
    uint32_t index{0};
};

/// The installed font the system picks to draw `codepoint`, in text whose
/// language is `language` (a BCP 47 tag such as "ja"; empty when unknown),
/// sans-serif where it has a choice. None when no installed font draws it,
/// or the system has no way to ask.
[[nodiscard]] std::optional<FontFile> fallback_font(char32_t codepoint, std::string_view language = {});

} // namespace ftk::platform
