#include "ftk/shader/shader_diagnostic.hpp"

#include <cctype>
#include <optional>

namespace ftk {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// The number after the last colon of `where`, taken off it; none when what
// follows that colon isn't one.
std::optional<int> take_number(std::string_view& where) {
    const auto colon = where.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::string_view number = where.substr(colon + 1);
    if (number.empty()) return std::nullopt;
    int value = 0;
    for (char c : number) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        value = value * 10 + (c - '0');
    }
    where = where.substr(0, colon);
    return value;
}

// "file:42: error: message" or "file:42:7: error: message" into its parts.
// The file may itself hold a colon (a Windows drive), so the line and column
// are read from the end of what comes before ": error:".
bool parse_error_line(std::string_view line, ShaderDiagnostic& out) {
    constexpr std::string_view MARK = ": error: ";
    const auto mark = line.find(MARK);
    if (mark == std::string_view::npos) return false;
    std::string_view where = line.substr(0, mark);
    const std::optional<int> last = take_number(where);
    if (!last) return false;
    const std::optional<int> before = take_number(where);
    out.file = std::string(where);
    out.line = before.value_or(*last);
    out.column = before ? *last : 0;
    out.message = std::string(trim(line.substr(mark + MARK.size())));
    out.count = 1;
    return true;
}

// Each line of `output` that isn't blank, trimmed.
template <typename Each>
void for_each_line(std::string_view output, Each&& each) {
    std::string_view rest = output;
    while (!rest.empty()) {
        const auto eol = rest.find('\n');
        const std::string_view line = trim(rest.substr(0, eol));
        rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 1);
        if (!line.empty()) each(line);
    }
}

// The text's first line as its one error, for text in no error's form.
std::string_view first_line(std::string_view output) {
    std::string_view first;
    for_each_line(output, [&](std::string_view line) {
        if (first.empty()) first = line;
    });
    return first;
}

} // namespace

ShaderDiagnostic first_shader_error(std::string_view output) {
    std::vector<ShaderDiagnostic> errors = shader_errors(output);
    if (errors.empty()) return {};
    ShaderDiagnostic first = std::move(errors.front());
    first.count = static_cast<int>(errors.size());
    return first;
}

std::vector<ShaderDiagnostic> shader_errors(std::string_view output) {
    std::vector<ShaderDiagnostic> errors;
    for_each_line(output, [&](std::string_view line) {
        ShaderDiagnostic parsed;
        if (parse_error_line(line, parsed)) errors.push_back(std::move(parsed));
    });
    if (errors.empty()) {
        if (const std::string_view first = first_line(output); !first.empty()) {
            errors.push_back({.message = std::string(first), .count = 1});
        }
    }
    return errors;
}

} // namespace ftk
