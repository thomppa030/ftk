#include "renderer/shader_diagnostic.hpp"

#include <cctype>

namespace fjell {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// "file:42: error: message" into its parts. The file may itself hold a
// colon (a Windows drive), so the line is the last number before ": error:".
bool parse_error_line(std::string_view line, ShaderDiagnostic& out) {
    constexpr std::string_view MARK = ": error: ";
    const auto mark = line.find(MARK);
    if (mark == std::string_view::npos) return false;
    const std::string_view where = line.substr(0, mark);
    const auto colon = where.rfind(':');
    if (colon == std::string_view::npos) return false;
    const std::string_view number = where.substr(colon + 1);
    if (number.empty()) return false;
    int value = 0;
    for (char c : number) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
    }
    out.file = std::string(where.substr(0, colon));
    out.line = value;
    out.message = std::string(trim(line.substr(mark + MARK.size())));
    return true;
}

} // namespace

ShaderDiagnostic first_shader_error(std::string_view output) {
    ShaderDiagnostic result;
    std::string_view first_text;
    std::string_view rest = output;
    while (!rest.empty()) {
        const auto eol = rest.find('\n');
        const std::string_view line = trim(rest.substr(0, eol));
        rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 1);
        if (line.empty()) continue;
        if (first_text.empty()) first_text = line;
        ShaderDiagnostic parsed;
        if (!parse_error_line(line, parsed)) continue;
        if (result.count == 0) result = std::move(parsed);
        ++result.count;
    }
    if (result.count == 0 && !first_text.empty()) {
        result.message = std::string(first_text);
        result.count = 1;
    }
    return result;
}

} // namespace fjell
