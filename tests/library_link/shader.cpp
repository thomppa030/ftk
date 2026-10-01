// Links ftk-shader alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it keys a source for the SPIR-V cache and reads a
// compiler error back, neither of which needs a GPU or glslc.

#include "ftk/base/log.hpp"
#include "ftk/shader/shader_compiler.hpp"
#include "ftk/shader/shader_diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    ftk::log::init({.level = spdlog::level::warn});

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "fjell_link_shader";
    const std::string source = (root / "x.frag").generic_string();
    std::filesystem::create_directories(root);
    std::ofstream(source) << "void main() {}\n";

    const ftk::ShaderCompiler compiler{root.generic_string(), (root / "generated").generic_string(), 1};
    const uint64_t key = compiler.cache_key({source, "x"});
    const ftk::ShaderDiagnostic error =
        ftk::first_shader_error("tint.frag:3: error: 'strenght' : undeclared identifier");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ftk::log::shutdown();
    const bool ran = key != 0 && error.line == 3;
    return ran ? 0 : 1;
}
