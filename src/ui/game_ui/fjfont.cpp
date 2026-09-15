#include "ui/game_ui/fjfont.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <system_error>

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr std::array<NamedCharset, 5> kNamedCharsets{{
    {"ascii",       {32, 126}},
    {"latin-1",     {160, 255}},
    {"latin-ext-a", {256, 383}},
    {"greek",       {880, 1023}},
    {"cyrillic",    {1024, 1279}},
}};

// The charset a recipe gets when it names none.
const std::vector<CharsetRange> kDefaultCharset{{32, 126}, {160, 255}};

bool parse_hex(std::string_view text, uint32_t& out) {
    if (text.empty() || text.size() > 6) return false;
    auto result = std::from_chars(text.data(), text.data() + text.size(), out, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

template <typename T>
bool write_pod(std::FILE* f, const T& value) {
    return std::fwrite(&value, sizeof(T), 1, f) == 1;
}

template <typename T>
bool write_all(std::FILE* f, const std::vector<T>& values) {
    if (values.empty()) return true;
    return std::fwrite(values.data(), sizeof(T), values.size(), f) == values.size();
}

template <typename T>
bool read_all(std::FILE* f, std::vector<T>& values, size_t count) {
    values.resize(count);
    if (count == 0) return true;
    return std::fread(values.data(), sizeof(T), count, f) == count;
}

struct FileCloser {
    void operator()(std::FILE* f) const { if (f) std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

} // namespace

std::span<const NamedCharset> named_charsets() {
    return kNamedCharsets;
}

Result<CharsetRange> parse_charset_entry(std::string_view entry) {
    for (const auto& named : kNamedCharsets) {
        if (entry == named.name) return named.range;
    }
    if (!entry.starts_with("U+")) {
        return make_error("'" + std::string(entry) + "' is neither a named charset nor a U+ range");
    }
    auto body = entry.substr(2);
    auto dash = body.find('-');
    CharsetRange range;
    if (dash == std::string_view::npos) {
        if (!parse_hex(body, range.first)) {
            return make_error("'" + std::string(entry) + "' is not a valid codepoint");
        }
        range.last = range.first;
    } else {
        if (!parse_hex(body.substr(0, dash), range.first) ||
            !parse_hex(body.substr(dash + 1), range.last)) {
            return make_error("'" + std::string(entry) + "' is not a valid codepoint range");
        }
        if (range.last < range.first) {
            return make_error("'" + std::string(entry) + "' ends before it starts");
        }
    }
    if (range.last > 0x10FFFF) {
        return make_error("'" + std::string(entry) + "' is beyond the last Unicode codepoint");
    }
    return range;
}

std::string charset_entry_text(CharsetRange range) {
    for (const auto& named : kNamedCharsets) {
        if (named.range.first == range.first && named.range.last == range.last) {
            return std::string(named.name);
        }
    }
    char buf[32];
    if (range.first == range.last) {
        std::snprintf(buf, sizeof(buf), "U+%04X", range.first);
    } else {
        std::snprintf(buf, sizeof(buf), "U+%04X-%04X", range.first, range.last);
    }
    return buf;
}

Result<> save_font_recipe(const fs::path& path, const FjFontRecipe& recipe) {
    nlohmann::ordered_json j;
    j["source"] = recipe.source;
    auto charset = nlohmann::ordered_json::array();
    for (const auto& range : recipe.charset) charset.push_back(charset_entry_text(range));
    j["charset"] = charset;
    j["cell_size"] = recipe.cell_size;
    // A whole number of pixels is written as one; 6.5 stays 6.5.
    if (recipe.px_range == std::floor(recipe.px_range)) {
        j["px_range"] = static_cast<int>(recipe.px_range);
    } else {
        j["px_range"] = recipe.px_range;
    }
    j["kerning"] = recipe.kerning;

    std::ofstream file(path);
    if (!file) return make_error("cannot write " + path.string());
    file << j.dump(4) << "\n";
    if (!file) return make_error("write failed for " + path.string());
    return {};
}

Result<FjFontRecipe> load_font_recipe(const fs::path& path, std::vector<std::string>* warnings) {
    std::ifstream file(path);
    if (!file) return make_error("cannot open " + path.string());
    std::stringstream buffer;
    buffer << file.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(buffer.str());
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(path.string() + ": " + e.what());
    }
    if (!j.is_object()) return make_error(path.string() + ": recipe is not a JSON object");

    auto warn = [&](const std::string& message) {
        if (warnings) warnings->push_back(path.string() + ": " + message);
    };

    FjFontRecipe recipe;
    if (!j.contains("source") || !j["source"].is_string() || j["source"].get<std::string>().empty()) {
        return make_error(path.string() + ": 'source' must name a font file");
    }
    recipe.source = j["source"].get<std::string>();

    if (j.contains("charset")) {
        if (!j["charset"].is_array()) {
            return make_error(path.string() + ": 'charset' must be a list of names or U+ ranges");
        }
        for (const auto& entry : j["charset"]) {
            if (!entry.is_string()) {
                warn("charset entry is not a string, ignored");
                continue;
            }
            auto parsed = parse_charset_entry(entry.get<std::string>());
            if (!parsed) {
                warn("charset " + parsed.error() + ", ignored");
                continue;
            }
            recipe.charset.push_back(*parsed);
        }
    } else {
        recipe.charset = kDefaultCharset;
    }

    if (j.contains("cell_size")) {
        if (!j["cell_size"].is_number_unsigned() || j["cell_size"].get<uint32_t>() < 8 ||
            j["cell_size"].get<uint32_t>() > 512) {
            return make_error(path.string() + ": 'cell_size' must be a whole number of pixels from 8 to 512");
        }
        recipe.cell_size = j["cell_size"].get<uint32_t>();
    }
    if (j.contains("px_range")) {
        if (!j["px_range"].is_number() || j["px_range"].get<float>() <= 0.0f ||
            j["px_range"].get<float>() * 2.0f >= static_cast<float>(recipe.cell_size)) {
            return make_error(path.string() + ": 'px_range' must be positive and less than half the cell size");
        }
        recipe.px_range = j["px_range"].get<float>();
    }
    if (j.contains("kerning")) {
        if (!j["kerning"].is_boolean()) {
            return make_error(path.string() + ": 'kerning' must be true or false");
        }
        recipe.kerning = j["kerning"].get<bool>();
    }

    static constexpr std::array<std::string_view, 5> known{"source", "charset", "cell_size",
                                                           "px_range", "kerning"};
    for (const auto& [key, value] : j.items()) {
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            warn("unknown key '" + key + "', ignored");
        }
    }
    return recipe;
}

std::vector<uint32_t> charset_codepoints(const FjFontRecipe& recipe) {
    std::vector<uint32_t> codepoints;
    for (const auto& range : recipe.charset) {
        for (uint32_t cp = range.first; cp <= range.last; ++cp) {
            codepoints.push_back(cp);
            if (cp == 0xFFFFFFFFu) break;
        }
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

std::string describe_codepoints(const std::vector<uint32_t>& codepoints, size_t limit) {
    std::string out;
    char buf[16];
    for (size_t i = 0; i < codepoints.size() && i < limit; ++i) {
        std::snprintf(buf, sizeof(buf), "%sU+%04X", i ? " " : "", codepoints[i]);
        out += buf;
    }
    if (codepoints.size() > limit) out += " ...";
    return out;
}

fs::path font_source_path(const fs::path& recipe_path, const FjFontRecipe& recipe) {
    return recipe_path.parent_path() / recipe.source;
}

fs::path font_atlas_cache_path(const fs::path& recipe_path) {
    return recipe_path.parent_path() / ".fjcache" / (recipe_path.filename().string() + ".atlas");
}

uint64_t font_source_hash(std::string_view recipe_text, std::span<const uint8_t> source_bytes) {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
    };
    for (char c : recipe_text) mix(static_cast<uint8_t>(c));
    for (uint8_t b : source_bytes) mix(b);
    return h;
}

Result<uint64_t> font_source_hash(const fs::path& recipe_path, const fs::path& source_path) {
    auto read = [](const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    };
    std::vector<uint8_t> recipe_bytes;
    std::vector<uint8_t> source_bytes;
    if (!read(recipe_path, recipe_bytes)) return make_error("cannot read " + recipe_path.string());
    if (!read(source_path, source_bytes)) return make_error("cannot read " + source_path.string());
    std::string_view recipe_text(reinterpret_cast<const char*>(recipe_bytes.data()), recipe_bytes.size());
    return font_source_hash(recipe_text, source_bytes);
}

Result<> write_font_atlas(const fs::path& path, BakedFontAtlas& atlas) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return make_error("cannot create " + path.parent_path().string() + ": " + ec.message());

    auto& h = atlas.header;
    h.magic = FJFONT_ATLAS_MAGIC;
    h.version = FJFONT_ATLAS_VERSION;
    h.page_count = 1;
    h.glyph_count = static_cast<uint32_t>(atlas.glyphs.size());
    h.kerning_count = static_cast<uint32_t>(atlas.kerning.size());
    h.glyph_table_offset = sizeof(FjFontAtlasHeader);
    h.kerning_offset = h.glyph_table_offset + atlas.glyphs.size() * sizeof(FjFontGlyph);
    h.pixels_offset = h.kerning_offset + atlas.kerning.size() * sizeof(FjFontKerning);

    // Written beside the target and renamed over it, so a crash mid-write
    // never leaves a truncated atlas that passes the timestamp check.
    auto temp = path;
    temp += ".tmp";
    FilePtr f(std::fopen(temp.string().c_str(), "wb"));
    if (!f) return make_error("cannot write " + temp.string());
    bool ok = write_pod(f.get(), h) && write_all(f.get(), atlas.glyphs) &&
              write_all(f.get(), atlas.kerning) && write_all(f.get(), atlas.pixels);
    f.reset();
    if (!ok) {
        fs::remove(temp, ec);
        return make_error("write failed for " + temp.string());
    }
    fs::rename(temp, path, ec);
    if (ec) return make_error("cannot replace " + path.string() + ": " + ec.message());
    return {};
}

Result<FjFontAtlasHeader> read_font_atlas_header(const fs::path& path) {
    FilePtr f(std::fopen(path.string().c_str(), "rb"));
    if (!f) return make_error("cannot open " + path.string());
    FjFontAtlasHeader h{};
    if (std::fread(&h, 1, FJFONT_ATLAS_HEADER_V1_SIZE, f.get()) != FJFONT_ATLAS_HEADER_V1_SIZE) {
        return make_error(path.string() + " is too short to be a font atlas");
    }
    if (h.magic != FJFONT_ATLAS_MAGIC) return make_error(path.string() + " is not a font atlas");
    if (h.version != FJFONT_ATLAS_VERSION) {
        return make_error(path.string() + " is atlas version " + std::to_string(h.version) +
                          ", this build reads version " + std::to_string(FJFONT_ATLAS_VERSION));
    }
    return h;
}

Result<BakedFontAtlas> read_font_atlas(const fs::path& path) {
    auto header = read_font_atlas_header(path);
    if (!header) return make_error(header.error());

    FilePtr f(std::fopen(path.string().c_str(), "rb"));
    if (!f) return make_error("cannot open " + path.string());
    BakedFontAtlas atlas;
    atlas.header = *header;
    const auto& h = atlas.header;

    size_t pixel_count = static_cast<size_t>(h.atlas_width) * h.atlas_height * 4;
    if (h.page_count != 1 || h.atlas_width == 0 || h.atlas_height == 0 || pixel_count == 0) {
        return make_error(path.string() + " has an empty atlas page");
    }
    auto seek = [&](uint64_t offset) { return std::fseek(f.get(), static_cast<long>(offset), SEEK_SET) == 0; };
    if (!seek(h.glyph_table_offset) || !read_all(f.get(), atlas.glyphs, h.glyph_count) ||
        !seek(h.kerning_offset) || !read_all(f.get(), atlas.kerning, h.kerning_count) ||
        !seek(h.pixels_offset) || !read_all(f.get(), atlas.pixels, pixel_count)) {
        return make_error(path.string() + " is truncated");
    }
    return atlas;
}

bool font_atlas_is_fresh(const fs::path& recipe_path, const fs::path& source_path,
                         const fs::path& cache_path, uint64_t source_hash) {
    std::error_code ec;
    auto cache_time = fs::last_write_time(cache_path, ec);
    if (ec) return false;
    auto recipe_time = fs::last_write_time(recipe_path, ec);
    if (ec || recipe_time > cache_time) return false;
    auto source_time = fs::last_write_time(source_path, ec);
    if (ec || source_time > cache_time) return false;
    auto header = read_font_atlas_header(cache_path);
    return header && header->source_hash == source_hash;
}

} // namespace fjell
