#pragma once

// How the language server protocol names a document and places a position
// in it: a file's URI, and a count along a line in UTF-8 bytes or UTF-16
// units, whichever the two sides agreed on.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ftk::jsonrpc {

/// The file URI of `path`, an absolute path, its bytes outside the letters,
/// digits, - . _ ~ and the slashes percent-encoded: file:///home/kay/a%20b.cpp,
/// file:///C%3A/x.cpp.
[[nodiscard]] std::string uri_of(const std::filesystem::path& path);

/// The path the file URI `uri` names; nothing when it names no local file.
[[nodiscard]] std::optional<std::filesystem::path> path_of(std::string_view uri);

/// How a position counts along a line: in UTF-8 bytes, or in UTF-16 units,
/// the protocol's default.
enum class Encoding : uint8_t { Utf8, Utf16 };

/// Where along `line` a count of `units` of `encoding` reaches, as a byte;
/// the end of the line when it is past it.
[[nodiscard]] std::size_t byte_at(std::string_view line, std::size_t units, Encoding encoding);

/// How many units of `encoding` the first `byte` bytes of `line` are.
[[nodiscard]] std::size_t units_at(std::string_view line, std::size_t byte, Encoding encoding);

} // namespace ftk::jsonrpc
