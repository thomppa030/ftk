#include "ftk/jsonrpc/documents.hpp"

#include "ftk/base/utf8.hpp"

#include <algorithm>
#include <format>

namespace ftk::jsonrpc {

namespace fs = std::filesystem;

namespace {

bool unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string uri_of(const fs::path& path) {
    std::u8string generic = path.generic_u8string();
    // A drive letter is a part of the path, after the authority's slash.
    if (!generic.starts_with(u8'/')) generic.insert(generic.begin(), u8'/');
    std::string uri = "file://";
    for (const char8_t c : generic) {
        const auto byte = static_cast<unsigned char>(c);
        if (unreserved(byte)) {
            uri += static_cast<char>(byte);
        } else {
            uri += std::format("%{:02X}", static_cast<unsigned>(byte));
        }
    }
    return uri;
}

std::optional<fs::path> path_of(std::string_view uri) {
    constexpr std::string_view SCHEME = "file:";
    if (uri.size() < SCHEME.size()) return std::nullopt;
    for (std::size_t i = 0; i < SCHEME.size(); ++i) {
        const char c = uri[i] >= 'A' && uri[i] <= 'Z' ? static_cast<char>(uri[i] - 'A' + 'a') : uri[i];
        if (c != SCHEME[i]) return std::nullopt;
    }
    uri.remove_prefix(SCHEME.size());
    if (uri.starts_with("//")) {
        uri.remove_prefix(2);
        const std::size_t slash = uri.find('/');
        const std::string_view authority = uri.substr(0, slash);
        if (!authority.empty() && authority != "localhost") return std::nullopt;
        uri = slash == std::string_view::npos ? std::string_view{} : uri.substr(slash);
    }
    std::u8string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int high = hex_value(uri[i + 1]);
            const int low = hex_value(uri[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char8_t>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        decoded += static_cast<char8_t>(uri[i]);
    }
    if (decoded.empty()) return std::nullopt;
#ifdef _WIN32
    // /C:/x is the path C:/x.
    if (decoded.size() >= 3 && decoded[0] == u8'/' && decoded[2] == u8':') decoded.erase(0, 1);
#endif
    return fs::path(decoded).make_preferred();
}

std::size_t byte_at(std::string_view line, std::size_t units, Encoding encoding) {
    if (encoding == Encoding::Utf8) return std::min(units, line.size());
    std::size_t at = 0;
    std::size_t counted = 0;
    while (at < line.size() && counted < units) {
        const uint32_t codepoint = utf8::next(line, at);
        counted += codepoint >= 0x10000 ? 2 : 1;
    }
    return at;
}

std::size_t units_at(std::string_view line, std::size_t byte, Encoding encoding) {
    if (encoding == Encoding::Utf8) return byte;
    std::size_t at = 0;
    std::size_t counted = 0;
    byte = std::min(byte, line.size());
    while (at < byte) {
        const uint32_t codepoint = utf8::next(line, at);
        counted += codepoint >= 0x10000 ? 2 : 1;
    }
    return counted;
}

} // namespace ftk::jsonrpc
