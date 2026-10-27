#pragma once

// How the Windows backend finds a program by its name: the names it may
// have, from the extensions PATHEXT lists, and the folders PATH lists. They
// hold for narrow and wide strings alike, and are kept free of Windows so
// they are tested on every system.

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::platform::detail {

/// `text` with its ASCII capitals made small.
template <typename Char>
[[nodiscard]] std::basic_string<Char> ascii_lower(std::basic_string_view<Char> text) {
    std::basic_string<Char> lower(text);
    for (Char& c : lower) {
        if (c >= Char('A') && c <= Char('Z')) c = Char(c - Char('A') + Char('a'));
    }
    return lower;
}

/// `list` cut at each `;`, the empty parts left out and each part taken
/// out of a pair of quotes around it, as Windows reads PATH and PATHEXT.
template <typename Char>
[[nodiscard]] std::vector<std::basic_string<Char>> semicolon_list(std::basic_string_view<Char> list) {
    std::vector<std::basic_string<Char>> parts;
    while (!list.empty()) {
        const std::size_t end = std::min(list.find(Char(';')), list.size());
        std::basic_string_view<Char> part = list.substr(0, end);
        if (part.size() >= 2 && part.front() == Char('"') && part.back() == Char('"')) part = part.substr(1, part.size() - 2);
        if (!part.empty()) parts.emplace_back(part);
        list.remove_prefix(std::min(end + 1, list.size()));
    }
    return parts;
}

/// The extensions a program is found under, from PATHEXT's `list`: those of
/// a file the backend starts, a program (.com, .exe) or a batch file (.bat,
/// .cmd), in small letters in the list's order. Windows' own list when
/// `list` names none: .COM;.EXE;.BAT;.CMD.
template <typename Char>
[[nodiscard]] std::vector<std::basic_string<Char>> program_extensions(std::basic_string_view<Char> list) {
    static constexpr std::string_view STARTED[] = {".com", ".exe", ".bat", ".cmd"};
    std::vector<std::basic_string<Char>> extensions;
    for (const std::basic_string<Char>& listed : semicolon_list<Char>(list)) {
        const std::basic_string<Char> extension = ascii_lower<Char>(listed);
        for (const std::string_view started : STARTED) {
            const std::basic_string<Char> wanted(started.begin(), started.end());
            if (extension == wanted && std::ranges::find(extensions, wanted) == extensions.end()) {
                extensions.push_back(wanted);
            }
        }
    }
    if (extensions.empty()) {
        for (const std::string_view started : STARTED) extensions.emplace_back(started.begin(), started.end());
    }
    return extensions;
}

/// The names the program `name`, a file's name without its folder, may be
/// found under: itself when its extension is one of `extensions`, in any
/// case; else itself with each of them added, in their order, as cmd adds
/// them to a name it is given.
template <typename Char>
[[nodiscard]] std::vector<std::basic_string<Char>> program_names(std::basic_string_view<Char> name,
                                                                 std::span<const std::basic_string<Char>> extensions) {
    if (name.empty()) return {};
    // A dot that starts the name begins no extension.
    const std::size_t dot = name.rfind(Char('.'));
    if (dot != std::basic_string_view<Char>::npos && dot > 0 &&
        std::ranges::find(extensions, ascii_lower<Char>(name.substr(dot))) != extensions.end()) {
        return {std::basic_string<Char>(name)};
    }
    std::vector<std::basic_string<Char>> names;
    for (const std::basic_string<Char>& extension : extensions) names.push_back(std::basic_string<Char>(name) + extension);
    return names;
}

} // namespace ftk::platform::detail
