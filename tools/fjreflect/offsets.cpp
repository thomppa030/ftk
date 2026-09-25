#include "parser.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace fjreflect {

// ── Offset query via compiler ───────────────────────────────────────────

bool query_offsets(std::vector<ComponentInfo>& components,
                   const std::string& source_file,
                   const std::vector<std::string>& include_dirs,
                   const std::string& compiler) {
    if (components.empty()) return true;

    // Generate a temporary C++ file that prints offsets
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto query_src = tmp_dir / "fjreflect_query.cpp";
    auto query_bin = tmp_dir / "fjreflect_query";

    {
        std::ofstream out(query_src);
        if (!out) {
            std::cerr << "fjreflect: cannot write " << query_src << "\n";
            return false;
        }

        // Block api.h from being included (it redefines our macros and
        // pulls in Windows SDK headers we don't need for offset queries).
        // Provide the minimal types api.h would have defined.
        out << "#define FJELL_SCRIPTING_API_H\n"; // api.h include guard
        out << "#define FJELL_SDK_EXPORT\n";
        out << "#define FJELL_GAME_EXPORT\n";
        out << "#define FJELL_API_VERSION 0\n";
        out << "#define FJELL_PRINTF_FMT(fmt_idx, va_idx)\n";
        out << "#include <cstdint>\n";
        out << "#include <cstddef>\n";
        // Mirrors FjellFieldType in fjell/api.h and must keep the same order.
        out << "enum FjellFieldType { FJELL_FIELD_FLOAT=0, FJELL_FIELD_INT, FJELL_FIELD_BOOL,\n"
               "    FJELL_FIELD_VEC2, FJELL_FIELD_VEC3, FJELL_FIELD_VEC4,\n"
               "    FJELL_FIELD_COLOR3, FJELL_FIELD_COLOR4, FJELL_FIELD_STRING,\n"
               "    FJELL_FIELD_ENUM, FJELL_FIELD_WEAK_REF, FJELL_FIELD_REF,\n"
               "    FJELL_FIELD_WIDGET_PTR, FJELL_FIELD_STD_STRING };\n";
        out << "struct FjellFieldDesc { const char* name; FjellFieldType type;\n"
               "    uint32_t offset; uint32_t size; const char* const* enum_labels;\n"
               "    float min; float max; float speed; const char* ref_type_name;\n"
               "    const char* tooltip; const char* category; uint8_t readonly; };\n";
        out << "enum FjellUiActionParam { FJELL_UI_ACTION_NONE=0, FJELL_UI_ACTION_INT,\n"
               "    FJELL_UI_ACTION_FLOAT, FJELL_UI_ACTION_VEC2 };\n";
        out << "struct FjellUiActionDesc { const char* name; FjellUiActionParam param_type; };\n";
        out << "enum FjellRpcTarget { FJELL_RPC_SERVER=0, FJELL_RPC_CLIENTS, FJELL_RPC_ALL };\n";
        out << "struct FjellRpcDesc { const char* name; FjellRpcTarget target;\n"
               "    uint32_t payload_size; void (*invoke)(void*, const uint8_t*, uint32_t); };\n";
        out << "struct FjellComponentLayout { const char* type_name; uint32_t size; };\n\n";

        // Stub engine extern "C" symbols that registration macros reference.
        // Headers only declare classes + fields, no engine method bodies.
        out << "struct FjellTypeRegistry;\n";
        out << "struct FjellComponentTypeOOP;\n";
        out << "struct FjellUiWidgetTypeOOP;\n";
        out << "using FjellUiComponentTypeOOP = FjellUiWidgetTypeOOP;\n";
        out << "struct FjellGeneratedInfo;\n";
        out << "struct FjellUiGeneratedInfo;\n";
        out << "struct FjellNotifyTypeOOP;\n";
        out << "extern \"C\" void fjell_reg_component(FjellTypeRegistry*, const FjellComponentTypeOOP*) {}\n";
        out << "extern \"C\" void fjell_reg_ui_component(FjellTypeRegistry*, const FjellUiWidgetTypeOOP*) {}\n";
        out << "extern \"C\" void fjell_reg_notify(FjellTypeRegistry*, const FjellNotifyTypeOOP*) {}\n";
        out << "extern \"C\" void fjell_set_generated_table(const FjellGeneratedInfo*, unsigned) {}\n";
        out << "extern \"C\" void fjell_set_ui_generated_table(const FjellUiGeneratedInfo*, unsigned) {}\n";
        out << "extern \"C\" const FjellUiGeneratedInfo* fjell_find_ui_generated(const char*) { return nullptr; }\n";
        out << "extern \"C\" const FjellGeneratedInfo* fjell_find_generated(const char*) { return nullptr; }\n";
        out << "extern \"C\" void* fjell_resolve_weak_ref(unsigned, unsigned) { return nullptr; }\n";
        out << "extern \"C\" void fjell_ref_acquire(unsigned) {}\n";
        out << "extern \"C\" void fjell_ref_release(unsigned) {}\n";
        out << "extern \"C\" void* fjell_ref_resolve(unsigned, unsigned char) { return nullptr; }\n";
        out << "extern \"C\" void fjell_shared_ref_acquire(unsigned) {}\n";
        out << "extern \"C\" void fjell_shared_ref_release(unsigned) {}\n";
        out << "extern \"C\" void* fjell_shared_ref_resolve(unsigned) { return nullptr; }\n";
        out << "extern \"C\" void fjell_gc_trace_weak(void*, void*) {}\n";
        out << "extern \"C\" void fjell_gc_trace_ref(void*, void*) {}\n";
        out << "extern \"C\" void fjell_gc_trace_shared(void*, void*) {}\n\n";

        out << "#include \"" << std::filesystem::path(source_file).filename().string() << "\"\n";
        out << "#include <cstdio>\n";
        out << "#include <cstddef>\n";
        out << "int main() {\n";

        for (const auto& comp : components) {
            for (const auto& f : comp.fields) {
                out << "    printf(\"" << comp.class_name << " " << f.name
                    << " %zu %zu\\n\", offsetof(" << comp.qualified_name << ", " << f.name
                    << "), sizeof(((" << comp.qualified_name << "*)nullptr)->" << f.name << "));\n";
            }
        }

        out << "    return 0;\n}\n";
    }

    // Build compiler command
    bool is_msvc = compiler.find("cl") != std::string::npos
                && compiler.find("clang") == std::string::npos;

    std::string includes;
    auto source_dir = std::filesystem::path(source_file).parent_path().generic_string();
    if (!source_dir.empty()) {
        if (is_msvc)
            includes += " /I\"" + source_dir + "\"";
        else
            includes += " -I" + source_dir;
    }
    for (const auto& dir : include_dirs) {
        if (is_msvc)
            includes += " /I\"" + dir + "\"";
        else
            includes += " -I" + dir;
    }

    std::string cmd;
    if (is_msvc) {
        auto query_exe = query_bin;
        query_exe += ".exe";
        // Derive vcvarsall.bat path from cl.exe path:
        // cl.exe is at .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe
        // vcvarsall is at .../VC/Auxiliary/Build/vcvarsall.bat
        auto cl_path = std::filesystem::path(compiler);
        // cl.exe: .../VC/Tools/MSVC/<ver>/bin/Host<arch>/<arch>/cl.exe
        //         7 parent_path() calls to reach .../VC
        auto vc_root = cl_path.parent_path();
        for (int i = 0; i < 6; ++i) vc_root = vc_root.parent_path();
        auto vcvarsall = vc_root / "Auxiliary" / "Build" / "vcvarsall.bat";

        // Write a temporary .bat file that sets up the MSVC environment
        // and compiles the query. _popen uses cmd.exe which makes quoting
        // paths with spaces unreliable; a .bat file avoids that entirely.
        auto bat_file = tmp_dir / "fjreflect_build.bat";
        {
            std::ofstream bat(bat_file);
            bat << "@echo off\n";
            if (std::filesystem::exists(vcvarsall)) {
                bat << "call \"" << vcvarsall.string() << "\" x64 >nul 2>&1\n";
            }
            // Use /MDd and _DEBUG to match Debug build's std::string/container
            // layout (iterator debugging changes sizeof(std::string) from 32 to 40).
            bat << "\"" << cl_path.string() << "\" /std:c++latest /w /EHsc /MDd /D_DEBUG"
                << includes
                << " /Fe\"" << query_exe.string() << "\""
                << " \"" << query_src.string() << "\""
                << " /link /NOLOGO /FORCE:UNRESOLVED\n";
        }
        cmd = "\"" + bat_file.string() + "\" 2>&1";
        query_bin = query_exe;
    } else {
        cmd = compiler + " -std=c++23 -Wno-attributes -Wno-invalid-offsetof -w"
            " -Wl,--unresolved-symbols=ignore-in-object-files"
            + includes + " -o " + query_bin.string() + " " + query_src.string() + " 2>&1";
    }

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "fjreflect: cannot run compiler\n";
        return false;
    }
    std::string compile_output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        compile_output += buf;
    }
    int status = pclose(pipe);
    if (status != 0) {
        std::cerr << "fjreflect: offset query compilation failed:\n" << compile_output;
        std::filesystem::remove(query_src);
        return false;
    }

    // Run
    pipe = popen(query_bin.string().c_str(), "r");
    if (!pipe) {
        std::cerr << "fjreflect: cannot run offset query\n";
        return false;
    }

    // Parse output: "ClassName fieldname offset size"
    std::string line;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::istringstream iss(buf);
        std::string class_name, field_name;
        size_t offset, size;
        if (iss >> class_name >> field_name >> offset >> size) {
            for (auto& comp : components) {
                if (comp.class_name != class_name) continue;
                for (auto& f : comp.fields) {
                    if (f.name == field_name) {
                        f.offset = static_cast<uint32_t>(offset);
                        f.size = static_cast<uint32_t>(size);
                    }
                }
            }
        }
    }
    pclose(pipe);

    // Cleanup
    std::filesystem::remove(query_src);
    std::filesystem::remove(query_bin);
    std::filesystem::remove(tmp_dir / "fjreflect_query.o");
    std::filesystem::remove(tmp_dir / "fjreflect_query.obj"); // MSVC object file
    std::filesystem::remove(tmp_dir / "fjreflect_build.bat"); // MSVC build script

    return true;
}

} // namespace fjreflect
