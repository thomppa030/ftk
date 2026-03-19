#pragma once

#include "ui/game_ui/ui_node.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace fjell {

/// Hand-written recursive descent parser for .fjui markup.
///
/// Syntax:
///   <tag id="foo" class="bar baz" attr="val">
///     <child/>
///     Some text
///   </tag>
class FjuiParser {
public:
    [[nodiscard]] static std::unique_ptr<UiNode> parse(std::string_view source);

private:
    explicit FjuiParser(std::string_view source);

    std::unique_ptr<UiNode> parse_document();
    std::unique_ptr<UiNode> parse_element();
    std::unique_ptr<UiNode> parse_text();
    void parse_attributes(UiNode& node);
    std::string parse_attribute_value();
    std::string parse_tag_name();

    void skip_whitespace();
    void skip_comment();
    [[nodiscard]] bool at_end() const;
    [[nodiscard]] char peek() const;
    char advance();
    bool expect(char c);

    std::string_view source_;
    size_t pos_{0};
    uint32_t line_{1};
};

} // namespace fjell
