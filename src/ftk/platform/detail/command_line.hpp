#pragma once

// A program's arguments as one Windows command line, written so the child
// reads each back as it was given by the rules every Windows program's
// runtime reads its command line by (CommandLineToArgvW's): an argument
// that is empty or holds a space, a tab or a quote goes in quotes, a quote
// in it is escaped with a backslash, and the backslashes before a quote
// are doubled. The rules hold for narrow and wide strings alike, and are
// kept free of Windows so they are tested on every system.

#include <span>
#include <string>
#include <string_view>

namespace ftk::platform::detail {

/// Appends `argument` to `line` as the child should read it back.
template <typename Char>
void append_argument(std::basic_string<Char>& line, std::basic_string_view<Char> argument) {
    static constexpr Char SPECIAL[] = {Char(' '), Char('\t'), Char('\n'), Char('\v'), Char('"'), Char(0)};
    if (!argument.empty() && argument.find_first_of(SPECIAL) == std::basic_string_view<Char>::npos) {
        line += argument;
        return;
    }
    line += Char('"');
    std::size_t backslashes = 0;
    for (const Char c : argument) {
        if (c == Char('\\')) {
            ++backslashes;
            continue;
        }
        // Backslashes are only special before a quote: there they are
        // doubled, and the quote itself escaped.
        line.append(c == Char('"') ? backslashes * 2 + 1 : backslashes, Char('\\'));
        backslashes = 0;
        line += c;
    }
    // Before the closing quote, backslashes are doubled too.
    line.append(backslashes * 2, Char('\\'));
    line += Char('"');
}

/// `arguments` as one command line, the program first.
template <typename Char>
[[nodiscard]] std::basic_string<Char> command_line(std::span<const std::basic_string<Char>> arguments) {
    std::basic_string<Char> line;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i > 0) line += Char(' ');
        append_argument<Char>(line, arguments[i]);
    }
    return line;
}

} // namespace ftk::platform::detail
