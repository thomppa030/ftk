#pragma once

// The language server protocol's base protocol: each message is a JSON
// text after a header that names its length in bytes,
//
//     Content-Length: 52\r\n
//     \r\n
//     {"jsonrpc":"2.0","id":1,"method":"initialize",...}
//
// framed for sending, and read back out of the bytes the other side writes,
// in whatever pieces a pipe hands them over.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ftk::jsonrpc {

/// `body` after its header, ready to write.
[[nodiscard]] std::string framed(std::string_view body);

/// Reads whole messages out of bytes as they come.
class FrameReader {
public:
    /// Takes in more of what the other side wrote.
    void add(std::string_view bytes);

    /// The body of the next message once all of it has come. A header with
    /// no length that can be read is passed over, with what follows it up
    /// to the next header.
    [[nodiscard]] std::optional<std::string> next();

private:
    std::string buffer_;
    // Where what is still unread begins.
    std::size_t start_{0};
};

} // namespace ftk::jsonrpc
