#pragma once

#include <string>
#include <string_view>

namespace ftk {

/// The first error in what the shader compiler said, for showing where the
/// shader is looked at rather than only in the log.
struct ShaderDiagnostic {
    /// The file the error is in: the file its author wrote for an error in
    /// its own code, "<generated>" for one in the template around it, empty
    /// when the text names no file.
    std::string file;
    /// 1-based, or 0 when the text names no line.
    int line{0};
    /// What is wrong, as the compiler put it ("'foam_bias' : undeclared
    /// identifier"), or the text's first line when it isn't a compiler error.
    std::string message;
    /// How many errors the text reports.
    int count{0};
};

/// Reads glslc's output ("sea.frag:42: error: 'foam_bias' : undeclared
/// identifier"). Text in any other form (a parser's own message) becomes the
/// message whole, first line only.
[[nodiscard]] ShaderDiagnostic first_shader_error(std::string_view output);

} // namespace ftk
