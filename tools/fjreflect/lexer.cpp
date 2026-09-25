#include "lexer.hpp"

#include <cctype>

namespace fjreflect {

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool is_string_prefix(std::string_view s) {
    return s == "u8" || s == "u" || s == "U" || s == "L" || s == "R" || s == "u8R" ||
           s == "uR" || s == "UR" || s == "LR";
}

bool is_char_prefix(std::string_view s) {
    return s == "u8" || s == "u" || s == "U" || s == "L";
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : src_(source) {}

    LexResult run() {
        bool line_start = true; // only whitespace so far on this line
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
                line_start = true;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++pos_;
                continue;
            }
            if (c == '/' && peek(1) == '/') {
                skip_line_comment();
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                skip_block_comment();
                continue;
            }
            if (c == '#' && line_start) {
                skip_directive();
                continue;
            }
            line_start = false;

            if (is_ident_start(c)) {
                lex_identifier();
            } else if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
                lex_number();
            } else if (c == '"') {
                lex_string(pos_, line_, false);
            } else if (c == '\'') {
                lex_character(pos_, line_);
            } else {
                lex_punct();
            }
        }
        out_.tokens.push_back({TokenKind::end, "", line_});
        return std::move(out_);
    }

private:
    [[nodiscard]] char peek(std::size_t ahead) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    void error(unsigned line, const std::string& message) {
        out_.errors.push_back(std::to_string(line) + ": " + message);
    }

    void emit(TokenKind kind, std::size_t begin, unsigned line) {
        out_.tokens.push_back({kind, std::string(src_.substr(begin, pos_ - begin)), line});
    }

    void skip_line_comment() {
        while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
    }

    void skip_block_comment() {
        const unsigned start_line = line_;
        pos_ += 2;
        while (pos_ < src_.size() && !(src_[pos_] == '*' && peek(1) == '/')) {
            if (src_[pos_] == '\n') ++line_;
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            error(start_line, "unterminated block comment");
            return;
        }
        pos_ += 2;
    }

    // A directive runs to the end of its line, and on past it while each line
    // ends in a backslash. Nothing in it is code the parser needs.
    void skip_directive() {
        while (pos_ < src_.size() && src_[pos_] != '\n') {
            const char c = src_[pos_];
            if (c == '\\' && peek(1) == '\n') {
                pos_ += 2;
                ++line_;
            } else if (c == '\\' && peek(1) == '\r' && peek(2) == '\n') {
                pos_ += 3;
                ++line_;
            } else if (c == '/' && peek(1) == '*') {
                skip_block_comment();
            } else if (c == '/' && peek(1) == '/') {
                skip_line_comment();
            } else {
                ++pos_;
            }
        }
    }

    void lex_identifier() {
        const std::size_t begin = pos_;
        const unsigned line = line_;
        while (pos_ < src_.size() && is_ident_char(src_[pos_])) ++pos_;
        const std::string_view word = src_.substr(begin, pos_ - begin);
        if (pos_ < src_.size() && src_[pos_] == '"' && is_string_prefix(word)) {
            lex_string(begin, line, word.back() == 'R');
            return;
        }
        if (pos_ < src_.size() && src_[pos_] == '\'' && is_char_prefix(word)) {
            lex_character(begin, line);
            return;
        }
        emit(TokenKind::identifier, begin, line);
    }

    // Digits, letters (hex digits, suffixes, exponents), dots, a sign right
    // after an exponent, and a quote between two digits (1'000'000).
    void lex_number() {
        const std::size_t begin = pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            const char prev = pos_ > begin ? src_[pos_ - 1] : '\0';
            if (is_ident_char(c) || c == '.') {
                ++pos_;
            } else if ((c == '+' || c == '-') &&
                       (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) {
                ++pos_;
            } else if (c == '\'' && is_ident_char(prev) && is_ident_char(peek(1))) {
                ++pos_;
            } else {
                break;
            }
        }
        emit(TokenKind::number, begin, line_);
    }

    // pos_ is at the opening quote; begin is where the prefix started.
    void lex_string(std::size_t begin, unsigned line, bool raw) {
        if (raw) {
            const std::size_t open = src_.find('(', pos_ + 1);
            if (open == std::string_view::npos || open - pos_ - 1 > 16) {
                error(line, "malformed raw string literal");
                pos_ = src_.size();
                return;
            }
            const std::string close =
                ")" + std::string(src_.substr(pos_ + 1, open - pos_ - 1)) + "\"";
            const std::size_t end = src_.find(close, open + 1);
            if (end == std::string_view::npos) {
                error(line, "unterminated raw string literal");
                pos_ = src_.size();
                return;
            }
            for (std::size_t i = pos_; i < end; ++i) {
                if (src_[i] == '\n') ++line_;
            }
            pos_ = end + close.size();
        } else {
            ++pos_;
            while (pos_ < src_.size() && src_[pos_] != '"' && src_[pos_] != '\n') {
                if (src_[pos_] == '\\' && peek(1) == '\n') ++line_;
                pos_ += src_[pos_] == '\\' ? 2 : 1;
            }
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                error(line, "unterminated string literal");
                return;
            }
            ++pos_;
        }
        while (pos_ < src_.size() && is_ident_char(src_[pos_])) ++pos_; // a literal suffix
        emit(TokenKind::string, begin, line);
    }

    void lex_character(std::size_t begin, unsigned line) {
        ++pos_;
        while (pos_ < src_.size() && src_[pos_] != '\'' && src_[pos_] != '\n') {
            pos_ += src_[pos_] == '\\' ? 2 : 1;
        }
        if (pos_ >= src_.size() || src_[pos_] != '\'') {
            error(line, "unterminated character literal");
            return;
        }
        ++pos_;
        emit(TokenKind::character, begin, line);
    }

    void lex_punct() {
        const std::size_t begin = pos_;
        const char c = src_[pos_];
        if ((c == ':' && peek(1) == ':') || (c == '-' && peek(1) == '>')) {
            pos_ += 2;
        } else if (c == '.' && peek(1) == '.' && peek(2) == '.') {
            pos_ += 3;
        } else {
            ++pos_;
        }
        emit(TokenKind::punct, begin, line_);
    }

    std::string_view src_;
    std::size_t pos_{0};
    unsigned line_{1};
    LexResult out_;
};

} // namespace

LexResult tokenize(std::string_view source) {
    return Lexer(source).run();
}

} // namespace fjreflect
