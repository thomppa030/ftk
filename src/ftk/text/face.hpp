#pragma once

// A face of a font file: which characters it draws, how tall its lines are
// at a size, and what shaping and rasterizing need from it. Loaded once and
// shared by everything that draws with it.
//
//     auto inter = text::Face::load(fonts / "Inter-Regular.ttf");
//     if (!inter) return inter.error();
//     const text::Metrics m = (*inter)->metrics(14.0f);
//
// A face is loaded and read from one thread at a time.

#include "ftk/base/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ftk::text {

/// A face's vertical measurements at a pixel size, each a distance from the
/// baseline: the ascent above it, the descent below it.
struct Metrics {
    float ascent{0.0f};
    float descent{0.0f};
    /// Extra room the font asks for between lines.
    float line_gap{0.0f};

    /// From one baseline to the next.
    [[nodiscard]] float line_height() const { return ascent + descent + line_gap; }
};

class Face {
public:
    /// The face at `index` of the font file at `path`; most files hold one.
    [[nodiscard]] static Result<std::shared_ptr<const Face>> load(const std::filesystem::path& path,
                                                                  uint32_t index = 0);
    /// The same from a font file's bytes, such as a font compiled into the
    /// program. `name` says which font it is in an error.
    [[nodiscard]] static Result<std::shared_ptr<const Face>> load(std::vector<std::byte> data, uint32_t index,
                                                                  const std::string& name);

    ~Face();
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;
    Face(Face&&) = delete;
    Face& operator=(Face&&) = delete;

    /// Unique among the faces loaded in this run, for caches keyed by face.
    [[nodiscard]] uint32_t id() const;

    /// Its family, as the file names it: "Inter".
    [[nodiscard]] const std::string& family() const;

    /// Whether it has a glyph for `codepoint`.
    [[nodiscard]] bool covers(char32_t codepoint) const;

    /// Its measurements at `size` pixels to the em.
    [[nodiscard]] Metrics metrics(float size) const;

    /// What the font measures itself in: units to the em.
    [[nodiscard]] float units_per_em() const;

    /// FreeType's and HarfBuzz's handles on it, for the rest of ftk-text
    /// (ftk/text/detail/face_impl.hpp).
    struct Impl;
    [[nodiscard]] const Impl& impl() const { return *impl_; }

private:
    explicit Face(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace ftk::text
