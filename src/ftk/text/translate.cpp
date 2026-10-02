#include "ftk/text/translate.hpp"

#include <nlohmann/json.hpp>

#include <format>

namespace ftk::text {

namespace {

Catalog& current() {
    static Catalog catalog;
    return catalog;
}

} // namespace

Result<Catalog> Catalog::from_json(std::string_view json) {
    const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return make_error("a catalogue is a JSON object");
    const auto language = parsed.find("language");
    if (language == parsed.end() || !language->is_string()) return make_error("a catalogue names its \"language\"");
    const auto strings = parsed.find("strings");
    if (strings == parsed.end() || !strings->is_object()) {
        return make_error("a catalogue's \"strings\" are an object of English strings and their translations");
    }

    Catalog catalog;
    catalog.language_ = language->get<std::string>();
    for (const auto& [english, translation] : strings->items()) {
        if (!translation.is_string()) return make_error(std::format("the translation of \"{}\" isn't a string", english));
        catalog.strings_.emplace(english, translation.get<std::string>());
    }
    return catalog;
}

const std::string* Catalog::find(std::string_view english) const {
    const auto found = strings_.find(english);
    return found != strings_.end() ? &found->second : nullptr;
}

void use_catalog(Catalog catalog) {
    current() = std::move(catalog);
}

const std::string& language() {
    static const std::string english = "en";
    return current().language().empty() ? english : current().language();
}

std::string_view tr(std::string_view english) {
    const std::string* translation = current().find(english);
    return translation != nullptr ? std::string_view(*translation) : english;
}

const char* tr(const char* english) {
    const std::string* translation = current().find(english);
    return translation != nullptr ? translation->c_str() : english;
}

} // namespace ftk::text
