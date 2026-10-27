#pragma once

// A program's arguments as one Windows command line, written so the child
// reads each back as it was given by the rules every Windows program's
// runtime reads its command line by (CommandLineToArgvW's): an argument
// that is empty or holds a space, a tab or a quote goes in quotes, a quote
// in it is escaped with a backslash, and the backslashes before a quote
// are doubled. A batch file is run by cmd.exe, which reads its line by rules
// of its own, so its arguments are written for cmd first. The rules hold for
// narrow and wide strings alike, and are kept free of Windows so they are
// tested on every system.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

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

/// Whether `program` names a batch file, which only cmd.exe runs: one whose
/// name ends in .bat or .cmd, in any case, past the dots and spaces Windows
/// leaves off a file's name.
template <typename Char>
[[nodiscard]] bool is_batch_file(std::basic_string_view<Char> program) {
    while (!program.empty() && (program.back() == Char('.') || program.back() == Char(' '))) program.remove_suffix(1);
    if (program.size() < 4 || program[program.size() - 4] != Char('.')) return false;
    const auto lower = [](Char c) { return c >= Char('A') && c <= Char('Z') ? Char(c - Char('A') + Char('a')) : c; };
    const Char first = lower(program[program.size() - 3]);
    const Char second = lower(program[program.size() - 2]);
    const Char third = lower(program[program.size() - 1]);
    return (first == Char('b') && second == Char('a') && third == Char('t')) ||
           (first == Char('c') && second == Char('m') && third == Char('d'));
}

/// Appends `argument` to `line`, a batch file's, so that cmd.exe acts on
/// nothing in it and the programs the batch file hands it on to read it back
/// as it was given. cmd acts on & | < > ^ ( ) outside quotes, so an argument
/// holding anything but ASCII letters, digits and #$*+-./:?@\_ goes in
/// quotes, as does an empty one and one ending in a backslash; a quote in
/// it is doubled, and the backslashes before a quote or the closing one are
/// doubled too. cmd expands %name% inside quotes as well: each % is written
/// as %%cd:~,% , which cmd makes back into the %, with nothing after it
/// starting a name. Nothing carries a line break or a NUL past cmd.
/// @return whether it could be written
template <typename Char>
[[nodiscard]] bool append_batch_argument(std::basic_string<Char>& line, std::basic_string_view<Char> argument) {
    static constexpr std::string_view PLAIN = "#$*+-./:?@\\_";
    bool quote = argument.empty() || argument.back() == Char('\\');
    for (const Char c : argument) {
        const auto code = static_cast<char32_t>(static_cast<std::make_unsigned_t<Char>>(c));
        if (code == U'\0' || code == U'\n' || code == U'\r') return false;
        const bool letter_or_digit =
            (code >= U'0' && code <= U'9') || (code >= U'a' && code <= U'z') || (code >= U'A' && code <= U'Z');
        if (!letter_or_digit && (code >= U'\x80' || PLAIN.find(static_cast<char>(code)) == std::string_view::npos)) {
            quote = true;
        }
    }
    if (quote) line += Char('"');
    std::size_t backslashes = 0;
    for (const Char c : argument) {
        if (c == Char('\\')) {
            ++backslashes;
        } else {
            if (c == Char('"')) {
                line.append(backslashes, Char('\\'));
                line += Char('"');
            } else if (c == Char('%')) {
                for (const char written : std::string_view("%%cd:~,")) line += Char(written);
            }
            backslashes = 0;
        }
        line += c;
    }
    if (quote) {
        line.append(backslashes, Char('\\'));
        line += Char('"');
    }
    return true;
}

/// The command line cmd.exe is given to run the batch file `script` with
/// `arguments` after it, each written by append_batch_argument(). /d leaves
/// out cmd's AutoRun, /e:ON lets %cd:~,% cut a name and /v:OFF leaves ! as
/// it is; /c takes off the line's first and last quote, so the batch file
/// and its arguments go in a pair of quotes of their own.
/// @return the line, or nothing for what no quoting carries past cmd: a
///         line break or a NUL in an argument, a quote in the script's name
///         or a backslash ending it
template <typename Char>
[[nodiscard]] std::optional<std::basic_string<Char>> batch_command_line(
    std::basic_string_view<Char> script, std::span<const std::basic_string<Char>> arguments) {
    if (script.empty() || script.find(Char('"')) != std::basic_string_view<Char>::npos || script.back() == Char('\\')) {
        return std::nullopt;
    }
    std::basic_string<Char> line;
    for (const char written : std::string_view("cmd.exe /e:ON /v:OFF /d /c \"\"")) line += Char(written);
    line += script;
    line += Char('"');
    for (const std::basic_string<Char>& argument : arguments) {
        line += Char(' ');
        if (!append_batch_argument<Char>(line, argument)) return std::nullopt;
    }
    line += Char('"');
    return line;
}

} // namespace ftk::platform::detail
