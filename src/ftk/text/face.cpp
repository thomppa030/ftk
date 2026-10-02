#include "ftk/text/face.hpp"

#include "ftk/text/detail/face_impl.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>

namespace ftk::text {

namespace {

// FreeType's library, shared by every face. Creating and freeing faces in it
// is not safe from two threads at once, so both happen under its lock.
struct FreeType {
    FT_Library library{nullptr};
    std::mutex lock;

    FreeType() { FT_Init_FreeType(&library); }
    ~FreeType() { FT_Done_FreeType(library); }
    FreeType(const FreeType&) = delete;
    FreeType& operator=(const FreeType&) = delete;
    FreeType(FreeType&&) = delete;
    FreeType& operator=(FreeType&&) = delete;
};

// Made on the first face's load, so it outlives every face loaded after.
FreeType& freetype() {
    static FreeType instance;
    return instance;
}

std::atomic<uint32_t> next_id{1};

} // namespace

Face::Face(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

Face::~Face() {
    hb_font_destroy(impl_->hb);
    std::lock_guard guard(freetype().lock);
    FT_Done_Face(impl_->ft);
}

Result<std::shared_ptr<const Face>> Face::load(const std::filesystem::path& path, uint32_t index) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return make_error(std::format("can't open the font {}", path.string()));
    std::vector<std::byte> data;
    std::transform(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(),
                   std::back_inserter(data), [](char c) { return static_cast<std::byte>(c); });
    return load(std::move(data), index, path.string());
}

Result<std::shared_ptr<const Face>> Face::load(std::vector<std::byte> data, uint32_t index,
                                               const std::string& name) {
    auto impl = std::make_unique<Impl>();
    impl->data = std::move(data);
    const auto* bytes = reinterpret_cast<const FT_Byte*>(impl->data.data());

    {
        FreeType& ft = freetype();
        std::lock_guard guard(ft.lock);
        if (ft.library == nullptr) return make_error("FreeType didn't start");
        const FT_Error error = FT_New_Memory_Face(ft.library, bytes, static_cast<FT_Long>(impl->data.size()),
                                                  static_cast<FT_Long>(index), &impl->ft);
        if (error != 0) return make_error(std::format("{} isn't a font FreeType reads (error {})", name, error));
    }
    if (impl->ft->family_name != nullptr) impl->family = impl->ft->family_name;

    hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(impl->data.data()),
                                     static_cast<unsigned int>(impl->data.size()), HB_MEMORY_MODE_READONLY,
                                     nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, index);
    hb_blob_destroy(blob);
    impl->units_per_em = static_cast<float>(hb_face_get_upem(face));
    impl->hb = hb_font_create(face);
    hb_face_destroy(face);
    hb_font_make_immutable(impl->hb);

    impl->id = next_id.fetch_add(1);
    return std::shared_ptr<const Face>(new Face(std::move(impl)));
}

uint32_t Face::id() const {
    return impl_->id;
}

const std::string& Face::family() const {
    return impl_->family;
}

bool Face::covers(char32_t codepoint) const {
    hb_codepoint_t glyph = 0;
    return hb_font_get_nominal_glyph(impl_->hb, codepoint, &glyph) != 0;
}

Metrics Face::metrics(float size) const {
    hb_font_extents_t extents{};
    hb_font_get_h_extents(impl_->hb, &extents);
    const float scale = size / impl_->units_per_em;
    return {.ascent = static_cast<float>(extents.ascender) * scale,
            .descent = static_cast<float>(-extents.descender) * scale,
            .line_gap = static_cast<float>(extents.line_gap) * scale};
}

float Face::units_per_em() const {
    return impl_->units_per_em;
}

} // namespace ftk::text
