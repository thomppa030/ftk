// The game SDK ships its own copy of ftk's delegate, in the engine's
// namespace, so game code builds without ftk. Nothing joins the two at
// compile time, and a fix made to one copy alone once corrupted memory in
// game code that only ever saw the other, so this keeps them the same code.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// The file's code line by line: comments and blank lines out, and the two
// names that differ on purpose, the namespace and the include guard, made one.
std::vector<std::string> code_of(const std::string& text) {
    std::string code = std::regex_replace(text, std::regex(R"(/\*[\s\S]*?\*/)"), "");
    code = std::regex_replace(code, std::regex(R"(//[^\n]*)"), "");
    code = std::regex_replace(code, std::regex(R"(\b(ftk|fjell)::)"), "NS::");
    code = std::regex_replace(code, std::regex(R"(\bnamespace (ftk|fjell)\b)"), "namespace NS");
    code = std::regex_replace(code, std::regex(R"(\b(FTK|FJELL)_DELEGATE_HPP\b)"), "GUARD");

    std::vector<std::string> lines;
    std::istringstream in(code);
    for (std::string line; std::getline(in, line);) {
        line = std::regex_replace(line, std::regex(R"(\s+$)"), "");
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST_CASE("The SDK's delegate is ftk's, in the engine's namespace", "[delegate][sdk]") {
    const auto ftk = code_of(read(FJELL_SOURCE_DIR "/src/ftk/base/delegate.hpp"));
    const auto sdk = code_of(read(FJELL_SOURCE_DIR "/src/scripting/fjell/delegate.hpp"));
    REQUIRE_FALSE(ftk.empty());

    for (size_t i = 0; i < ftk.size() && i < sdk.size(); ++i) {
        INFO("first difference, line " << i + 1 << " of the code");
        REQUIRE(ftk[i] == sdk[i]);
    }
    CHECK(ftk.size() == sdk.size());
}
