#pragma once

// The strings a program shows go through a lookup, so they can be
// translated (decision 11): written in English, looked up in the catalogue
// of the language in use, and shown in English where it has no entry.
//
//     if (auto german = text::Catalog::from_json(read("de.json"))) text::use_catalog(*std::move(german));
//     ui::button(text::tr("Save scene"));
//
// A catalogue is JSON naming its language and each English string's
// translation:
//
//     {"language": "de", "strings": {"Save scene": "Szene speichern"}}
//
// It is set before the UI runs and read on the UI's thread.

#include "ftk/base/result.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ftk::text {

class Catalog {
public:
    /// An empty catalogue, which leaves everything in English.
    Catalog() = default;

    /// A catalogue from its JSON, or why it can't be read.
    [[nodiscard]] static Result<Catalog> from_json(std::string_view json);

    /// The language it translates into, as a BCP 47 tag: "de", "ja".
    [[nodiscard]] const std::string& language() const { return language_; }

    /// The translation of `english`, or none.
    [[nodiscard]] const std::string* find(std::string_view english) const;

    [[nodiscard]] std::size_t size() const { return strings_.size(); }

private:
    // Looked up by string_view without building a string.
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
    };

    std::string language_;
    std::unordered_map<std::string, std::string, Hash, std::equal_to<>> strings_;
};

/// Makes `catalog` the one tr() reads. What tr() returned before is no
/// longer valid.
void use_catalog(Catalog catalog);

/// The language in use, as a BCP 47 tag: "en" when no catalogue names one.
[[nodiscard]] const std::string& language();

/// `english` in the language in use: its translation, or itself.
[[nodiscard]] std::string_view tr(std::string_view english);

/// The same for a C string, as the kit's labels are: the translation's
/// characters, or `english` itself.
[[nodiscard]] const char* tr(const char* english);

} // namespace ftk::text
