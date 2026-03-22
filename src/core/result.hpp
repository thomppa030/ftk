#pragma once

#include <expected>
#include <string>

namespace fjell {

/// Rust-style Result type. Success holds T, failure holds a string message.
/// For void results, use Result<void> (or just Result<>).
template <typename T = void>
struct [[nodiscard]] Result : std::expected<T, std::string> {
    using std::expected<T, std::string>::expected;
};

/// Shorthand for creating an error result.
[[nodiscard]] inline std::unexpected<std::string> make_error(std::string msg) {
    return std::unexpected(std::move(msg));
}

} // namespace fjell
