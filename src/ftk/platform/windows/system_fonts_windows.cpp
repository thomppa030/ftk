#include "ftk/platform/system_fonts.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <dwrite_2.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

namespace ftk::platform {

namespace {

using Microsoft::WRL::ComPtr;

// The text DirectWrite's fallback reads: the one character asked about, in
// the locale asked for. It lives on the stack for one question, so its
// reference count is kept only to satisfy COM.
class OneCharacter final : public IDWriteTextAnalysisSource {
public:
    OneCharacter(const wchar_t* text, UINT32 length, const wchar_t* locale)
        : text_{text}, length_{length}, locale_{locale} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextAnalysisSource)) {
            *object = static_cast<IDWriteTextAnalysisSource*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text, UINT32* length) override {
        *text = position < length_ ? text_ + position : nullptr;
        *length = position < length_ ? length_ - position : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text, UINT32* length) override {
        *text = position > 0 && position <= length_ ? text_ : nullptr;
        *length = position > 0 && position <= length_ ? position : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length, const WCHAR** locale) override {
        *locale = locale_;
        *length = position < length_ ? length_ - position : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 position, UINT32* length,
                                                    IDWriteNumberSubstitution** substitution) override {
        *substitution = nullptr;
        *length = position < length_ ? length_ - position : 0;
        return S_OK;
    }

private:
    const wchar_t* text_;
    UINT32 length_;
    const wchar_t* locale_;
    ULONG references_{1};
};

std::mutex lock;

// The system's font fallback, made on the first question; null when this
// Windows has none (it came with Windows 8.1).
IDWriteFontFallback* system_fallback() {
    static ComPtr<IDWriteFontFallback> fallback = [] {
        ComPtr<IDWriteFactory2> factory;
        ComPtr<IDWriteFontFallback> made;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                          reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
            factory->GetSystemFontFallback(made.GetAddressOf());
        }
        return made;
    }();
    return fallback.Get();
}

// The path of a font face's first file, when it is a file on disk.
std::optional<std::filesystem::path> file_of(IDWriteFontFace* face) {
    UINT32 count = 0;
    if (FAILED(face->GetFiles(&count, nullptr)) || count == 0) return std::nullopt;
    std::vector<IDWriteFontFile*> files(count, nullptr);
    if (FAILED(face->GetFiles(&count, files.data()))) return std::nullopt;
    std::optional<std::filesystem::path> path;
    const void* key = nullptr;
    UINT32 key_size = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (SUCCEEDED(files[0]->GetReferenceKey(&key, &key_size)) && SUCCEEDED(files[0]->GetLoader(&loader)) &&
        SUCCEEDED(loader.As(&local))) {
        UINT32 length = 0;
        if (SUCCEEDED(local->GetFilePathLengthFromKey(key, key_size, &length))) {
            std::wstring wide(length + 1, L'\0');
            if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, wide.data(), length + 1))) {
                wide.resize(length);
                path = std::filesystem::path(wide);
            }
        }
    }
    for (IDWriteFontFile* file : files) file->Release();
    return path;
}

} // namespace

std::optional<FontFile> fallback_font(char32_t codepoint, std::string_view language) {
    std::lock_guard guard(lock);
    IDWriteFontFallback* fallback = system_fallback();
    if (fallback == nullptr) return std::nullopt;

    // The character in UTF-16, and the language widened: a BCP 47 tag is
    // ASCII.
    wchar_t text[2] = {};
    UINT32 length = 1;
    if (codepoint > 0xFFFF) {
        const char32_t offset = codepoint - 0x10000;
        text[0] = static_cast<wchar_t>(0xD800 + (offset >> 10));
        text[1] = static_cast<wchar_t>(0xDC00 + (offset & 0x3FF));
        length = 2;
    } else {
        text[0] = static_cast<wchar_t>(codepoint);
    }
    std::wstring locale = language.empty() ? std::wstring(L"en-us") : std::wstring(language.begin(), language.end());

    OneCharacter source(text, length, locale.c_str());
    UINT32 mapped = 0;
    ComPtr<IDWriteFont> font;
    FLOAT scale = 1.0f;
    if (FAILED(fallback->MapCharacters(&source, 0, length, nullptr, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mapped,
                                       font.GetAddressOf(), &scale)) ||
        font == nullptr || mapped == 0) {
        return std::nullopt;
    }
    ComPtr<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(face.GetAddressOf()))) return std::nullopt;
    const std::optional<std::filesystem::path> path = file_of(face.Get());
    if (!path) return std::nullopt;
    return FontFile{.path = *path, .index = face->GetIndex()};
}

} // namespace ftk::platform
