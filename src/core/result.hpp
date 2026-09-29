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

/// Moves the value of `expr`, a `Result`, into `target`, or returns its error
/// from the function it is in, which returns a `Result` too.
///
/// @code
/// FJELL_TRY(pipeline_, device.create(gpu::ComputePipelineDesc{...}));
/// @endcode
#define FJELL_TRY(target, expr)                                                   \
    do {                                                                          \
        auto fjell_try_result = (expr);                                           \
        if (!fjell_try_result) return std::unexpected(std::move(fjell_try_result).error()); \
        (target) = std::move(*fjell_try_result);                                  \
    } while (false)
