#include "ftk/jsonrpc/frames.hpp"

#include <charconv>
#include <format>

namespace ftk::jsonrpc {

namespace {

constexpr std::string_view LENGTH = "content-length:";
constexpr std::string_view END_OF_HEADER = "\r\n\r\n";

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The length a header names, read from its Content-Length line; its other
// lines (Content-Type) say nothing the reader needs.
std::optional<std::size_t> length_in(std::string_view header) {
    while (!header.empty()) {
        const std::size_t end = header.find("\r\n");
        std::string_view line = header.substr(0, end);
        header = end == std::string_view::npos ? std::string_view{} : header.substr(end + 2);
        if (line.size() < LENGTH.size()) continue;
        bool named = true;
        for (std::size_t i = 0; i < LENGTH.size() && named; ++i) named = lower(line[i]) == LENGTH[i];
        if (!named) continue;
        line.remove_prefix(LENGTH.size());
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        std::size_t length = 0;
        const auto read = std::from_chars(line.data(), line.data() + line.size(), length);
        if (read.ec == std::errc{}) return length;
    }
    return std::nullopt;
}

} // namespace

std::string framed(std::string_view body) {
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

void FrameReader::add(std::string_view bytes) {
    // What was read goes once it is most of what is held.
    if (start_ > 0 && start_ >= buffer_.size() / 2) {
        buffer_.erase(0, start_);
        start_ = 0;
    }
    buffer_ += bytes;
}

std::optional<std::string> FrameReader::next() {
    for (;;) {
        const std::size_t end = buffer_.find(END_OF_HEADER, start_);
        if (end == std::string::npos) return std::nullopt;
        const std::size_t body = end + END_OF_HEADER.size();
        const std::optional<std::size_t> length =
            length_in(std::string_view(buffer_).substr(start_, end - start_));
        if (!length) {
            start_ = body;
            continue;
        }
        if (buffer_.size() - body < *length) return std::nullopt;
        std::string message = buffer_.substr(body, *length);
        start_ = body + *length;
        return message;
    }
}

} // namespace ftk::jsonrpc
