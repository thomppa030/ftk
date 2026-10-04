#include "ftk/platform/system_fonts.hpp"

#include "ftk/platform/shared_library.hpp"

#include <mutex>
#include <string>
#include <type_traits>

namespace ftk::platform {

namespace {

// fontconfig is asked through the library the system has, loaded on the first
// question, so building ftk does not need its headers or its development
// package. These are the few parts of its C interface that are used, as
// fontconfig.h declares them.
struct FcConfig;
struct FcPattern;
struct FcCharSet;
struct FcFontSet {
    int nfont;
    int sfont;
    FcPattern** fonts;
};
using FcChar8 = unsigned char;
using FcChar32 = unsigned int;
using FcBool = int;
enum FcResult : int { FcResultMatch = 0 };
enum FcMatchKind : int { FcMatchPattern = 0 };

struct Fontconfig {
    SharedLibrary library;
    FcConfig* config{nullptr};

    FcConfig* (*init_load_config_and_fonts)(){nullptr};
    FcPattern* (*pattern_create)(){nullptr};
    void (*pattern_destroy)(FcPattern*){nullptr};
    FcCharSet* (*charset_create)(){nullptr};
    void (*charset_destroy)(FcCharSet*){nullptr};
    FcBool (*charset_add_char)(FcCharSet*, FcChar32){nullptr};
    FcBool (*charset_has_char)(const FcCharSet*, FcChar32){nullptr};
    FcBool (*pattern_add_charset)(FcPattern*, const char*, const FcCharSet*){nullptr};
    FcBool (*pattern_add_string)(FcPattern*, const char*, const FcChar8*){nullptr};
    FcBool (*pattern_add_bool)(FcPattern*, const char*, FcBool){nullptr};
    FcBool (*config_substitute)(FcConfig*, FcPattern*, FcMatchKind){nullptr};
    void (*default_substitute)(FcPattern*){nullptr};
    FcFontSet* (*font_sort)(FcConfig*, FcPattern*, FcBool, FcCharSet**, FcResult*){nullptr};
    void (*font_set_destroy)(FcFontSet*){nullptr};
    FcResult (*pattern_get_string)(const FcPattern*, const char*, int, FcChar8**){nullptr};
    FcResult (*pattern_get_integer)(const FcPattern*, const char*, int, int*){nullptr};
    FcResult (*pattern_get_charset)(const FcPattern*, const char*, int, FcCharSet**){nullptr};

    // Loads the library and its functions, and reads the system's font
    // configuration. False when any of it is missing.
    bool load() {
        if (!library.load("libfontconfig.so.1")) return false;
        bool found = true;
        const auto find = [&](auto& function, const char* name) {
            function = library.symbol_as<std::remove_reference_t<decltype(function)>>(name);
            found = found && function != nullptr;
        };
        find(init_load_config_and_fonts, "FcInitLoadConfigAndFonts");
        find(pattern_create, "FcPatternCreate");
        find(pattern_destroy, "FcPatternDestroy");
        find(charset_create, "FcCharSetCreate");
        find(charset_destroy, "FcCharSetDestroy");
        find(charset_add_char, "FcCharSetAddChar");
        find(charset_has_char, "FcCharSetHasChar");
        find(pattern_add_charset, "FcPatternAddCharSet");
        find(pattern_add_string, "FcPatternAddString");
        find(pattern_add_bool, "FcPatternAddBool");
        find(config_substitute, "FcConfigSubstitute");
        find(default_substitute, "FcDefaultSubstitute");
        find(font_sort, "FcFontSort");
        find(font_set_destroy, "FcFontSetDestroy");
        find(pattern_get_string, "FcPatternGetString");
        find(pattern_get_integer, "FcPatternGetInteger");
        find(pattern_get_charset, "FcPatternGetCharSet");
        if (!found) return false;
        config = init_load_config_and_fonts();
        if (config == nullptr) return false;
        // fontconfig keeps what it reads for as long as the process runs,
        // pointed at from its own globals; unloading it at exit would leave
        // that allocated with nothing pointing at it.
        library.keep_loaded();
        return true;
    }
};

std::mutex lock;

// fontconfig as loaded on the first question, or null when the system has
// none. Reading the configuration and font list takes a while, so it is done
// once.
Fontconfig* fontconfig() {
    static Fontconfig* loaded = [] {
        static Fontconfig instance;
        return instance.load() ? &instance : nullptr;
    }();
    return loaded;
}

} // namespace

std::optional<FontFile> fallback_font(char32_t codepoint, std::string_view language) {
    std::lock_guard guard(lock);
    Fontconfig* fc = fontconfig();
    if (fc == nullptr) return std::nullopt;

    FcPattern* pattern = fc->pattern_create();
    FcCharSet* wanted = fc->charset_create();
    fc->charset_add_char(wanted, codepoint);
    fc->pattern_add_charset(pattern, "charset", wanted);
    fc->pattern_add_string(pattern, "family", reinterpret_cast<const FcChar8*>("sans-serif"));
    fc->pattern_add_bool(pattern, "scalable", 1);
    if (!language.empty()) {
        const std::string tag(language);
        fc->pattern_add_string(pattern, "lang", reinterpret_cast<const FcChar8*>(tag.c_str()));
    }
    fc->config_substitute(fc->config, pattern, FcMatchPattern);
    fc->default_substitute(pattern);

    // fontconfig's fonts best first: the first that draws the character and
    // is a file ftk's FreeType reads is taken. FreeType is built without
    // compressed fonts, so WOFF and WOFF2 files are passed over.
    std::optional<FontFile> found;
    FcResult result = FcResultMatch;
    if (FcFontSet* fonts = fc->font_sort(fc->config, pattern, 1, nullptr, &result)) {
        for (int i = 0; i < fonts->nfont && !found; ++i) {
            FcPattern* font = fonts->fonts[i];
            FcCharSet* covered = nullptr;
            FcChar8* file = nullptr;
            int index = 0;
            if (fc->pattern_get_charset(font, "charset", 0, &covered) != FcResultMatch ||
                fc->charset_has_char(covered, codepoint) == 0 ||
                fc->pattern_get_string(font, "file", 0, &file) != FcResultMatch) {
                continue;
            }
            const std::filesystem::path path = reinterpret_cast<const char*>(file);
            if (path.extension() == ".woff" || path.extension() == ".woff2") continue;
            fc->pattern_get_integer(font, "index", 0, &index);
            found = FontFile{.path = path, .index = static_cast<uint32_t>(index)};
        }
        fc->font_set_destroy(fonts);
    }
    fc->pattern_destroy(pattern);
    fc->charset_destroy(wanted);
    return found;
}

} // namespace ftk::platform
