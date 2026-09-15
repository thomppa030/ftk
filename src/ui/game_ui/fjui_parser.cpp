#include "ui/game_ui/fjui_parser.hpp"
#include "core/log.hpp"

#include <cctype>
#include <sstream>

namespace fjell {

FjuiParser::FjuiParser(std::string_view source) : source_{source} {}

std::unique_ptr<UiNode> FjuiParser::parse(std::string_view source) {
    FjuiParser parser{source};
    return parser.parse_document();
}

std::unique_ptr<UiNode> FjuiParser::parse_document() {
    skip_whitespace();
    if (at_end()) return nullptr;

    // Create a root node that wraps all top-level elements
    auto root = std::make_unique<UiNode>();
    root->type = UiNodeType::root;
    root->tag = "root";

    while (!at_end()) {
        skip_whitespace();
        if (at_end()) break;

        if (peek() == '<') {
            // Skip comments
            if (pos_ + 3 < source_.size() &&
                source_[pos_ + 1] == '!' &&
                source_[pos_ + 2] == '-' &&
                source_[pos_ + 3] == '-') {
                skip_comment();
                continue;
            }
            // Skip closing tags at root level (shouldn't happen in valid markup)
            if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') break;

            auto child = parse_element();
            if (!child) return nullptr; // malformed element — fail fast
            child->parent = root.get();
            root->children.push_back(std::move(child));
        } else {
            auto text = parse_text();
            if (text && !text->text_content.empty()) {
                text->parent = root.get();
                root->children.push_back(std::move(text));
            }
        }
    }

    // If there's only one top-level element, promote it to root
    if (root->children.size() == 1 && root->children[0]->type == UiNodeType::element) {
        auto child = std::move(root->children[0]);
        child->parent = nullptr;
        return child;
    }

    return root;
}

std::unique_ptr<UiNode> FjuiParser::parse_element() {
    if (!expect('<')) return nullptr;

    auto node = std::make_unique<UiNode>();
    node->type = UiNodeType::element;
    node->source_line = line_;
    node->tag = parse_tag_name();

    if (node->tag.empty()) {
        FJELL_CORE_WARN("fjui: empty tag name at line {}", line_);
        return nullptr;
    }

    parse_attributes(*node);

    skip_whitespace();

    // Self-closing tag
    if (!at_end() && peek() == '/') {
        advance(); // /
        expect('>');
        return node;
    }

    if (!expect('>')) return node;

    // Parse children until closing tag
    while (!at_end()) {
        skip_whitespace();
        if (at_end()) break;

        if (peek() == '<') {
            // Comment
            if (pos_ + 3 < source_.size() &&
                source_[pos_ + 1] == '!' &&
                source_[pos_ + 2] == '-' &&
                source_[pos_ + 3] == '-') {
                skip_comment();
                continue;
            }

            // Closing tag
            if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
                advance(); // <
                advance(); // /
                auto close_tag = parse_tag_name();
                skip_whitespace();
                expect('>');
                if (close_tag != node->tag) {
                    FJELL_CORE_WARN("fjui: mismatched closing tag </{}>, expected </{}> at line {}",
                                    close_tag, node->tag, line_);
                }
                break;
            }

            // Child element
            auto child = parse_element();
            if (child) {
                child->parent = node.get();
                node->children.push_back(std::move(child));
            }
        } else {
            // Text content
            auto text = parse_text();
            if (text && !text->text_content.empty()) {
                text->parent = node.get();
                node->children.push_back(std::move(text));
            }
        }
    }

    return node;
}

std::unique_ptr<UiNode> FjuiParser::parse_text() {
    std::string content;
    while (!at_end() && peek() != '<') {
        if (peek() == '\n') line_++;
        content += advance();
    }

    // Trim whitespace
    auto start = content.find_first_not_of(" \t\n\r");
    auto end = content.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return nullptr;
    content = content.substr(start, end - start + 1);

    auto node = std::make_unique<UiNode>();
    node->type = UiNodeType::text;
    node->tag = "text";
    node->text_content = std::move(content);
    return node;
}

void FjuiParser::parse_attributes(UiNode& node) {
    while (!at_end()) {
        skip_whitespace();
        if (at_end() || peek() == '>' || peek() == '/') break;

        auto name = parse_tag_name();
        if (name.empty()) break;

        skip_whitespace();
        if (at_end() || peek() != '=') {
            // Boolean attribute
            node.attributes[name] = "true";
            continue;
        }
        advance(); // =
        skip_whitespace();

        auto value = parse_attribute_value();

        if (name == "id") {
            node.id = value;
        } else if (name == "class") {
            // Split space-separated classes
            std::istringstream iss(value);
            std::string cls;
            while (iss >> cls) {
                node.classes.push_back(cls);
            }
        } else {
            node.attributes[name] = value;
        }
    }
}

std::string FjuiParser::parse_attribute_value() {
    if (at_end()) return {};

    char quote = peek();
    if (quote != '"' && quote != '\'') {
        // Unquoted value (up to whitespace or >)
        std::string val;
        while (!at_end() && !std::isspace(static_cast<unsigned char>(peek())) &&
               peek() != '>' && peek() != '/') {
            val += advance();
        }
        return val;
    }

    advance(); // opening quote
    std::string val;
    while (!at_end() && peek() != quote) {
        if (peek() == '\n') line_++;
        val += advance();
    }
    if (!at_end()) advance(); // closing quote
    return val;
}

std::string FjuiParser::parse_tag_name() {
    std::string name;
    while (!at_end() &&
           (std::isalnum(static_cast<unsigned char>(peek())) ||
            peek() == '-' || peek() == '_' || peek() == ':')) {
        name += advance();
    }
    return name;
}

void FjuiParser::skip_whitespace() {
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
        if (peek() == '\n') line_++;
        advance();
    }
}

void FjuiParser::skip_comment() {
    // Skip <!-- ... -->
    pos_ += 4; // skip <!--
    while (pos_ + 2 < source_.size()) {
        if (source_[pos_] == '-' && source_[pos_ + 1] == '-' && source_[pos_ + 2] == '>') {
            pos_ += 3;
            return;
        }
        if (source_[pos_] == '\n') line_++;
        pos_++;
    }
    pos_ = source_.size();
}

bool FjuiParser::at_end() const {
    return pos_ >= source_.size();
}

char FjuiParser::peek() const {
    return source_[pos_];
}

char FjuiParser::advance() {
    return source_[pos_++];
}

bool FjuiParser::expect(char c) {
    if (at_end() || peek() != c) {
        FJELL_CORE_WARN("fjui: expected '{}' at line {}", c, line_);
        return false;
    }
    advance();
    return true;
}

} // namespace fjell
