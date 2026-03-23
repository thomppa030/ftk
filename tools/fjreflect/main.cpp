#include "parser.hpp"
#include "generator.hpp"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr << "Usage: fjreflect [options] <source files...>\n"
              << "Options:\n"
              << "  --output-dir <dir>    Output directory for generated files\n"
              << "  --include-dir <dir>   Additional include directory (can repeat)\n"
              << "  --compiler <path>     C++ compiler to use (default: c++)\n"
              << "  --help                Show this message\n";
}

int main(int argc, char* argv[]) {
    std::string output_dir = "generated";
    std::string compiler = "c++";
    std::vector<std::string> include_dirs;
    std::vector<std::string> source_files;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--include-dir") == 0 && i + 1 < argc) {
            include_dirs.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--compiler") == 0 && i + 1 < argc) {
            compiler = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            source_files.push_back(argv[i]);
        } else {
            std::cerr << "fjreflect: unknown option: " << argv[i] << "\n";
            print_usage();
            return 1;
        }
    }

    if (source_files.empty()) {
        std::cerr << "fjreflect: no source files\n";
        print_usage();
        return 1;
    }

    std::vector<fjreflect::ComponentInfo> all_components;
    bool had_errors = false;

    for (const auto& file : source_files) {
        if (!std::filesystem::exists(file)) {
            std::cerr << "fjreflect: file not found: " << file << "\n";
            had_errors = true;
            continue;
        }

        auto result = fjreflect::parse_file(file);

        for (const auto& w : result.warnings) {
            std::cerr << w << "\n";
        }
        for (const auto& e : result.errors) {
            std::cerr << e << "\n";
            had_errors = true;
        }

        // Query offsets for components found in this file
        if (!result.components.empty() && !had_errors) {
            if (!fjreflect::query_offsets(result.components, file, include_dirs, compiler)) {
                std::cerr << "fjreflect: offset query failed for " << file << "\n";
                had_errors = true;
            }
        }

        for (auto& comp : result.components) {
            std::cout << "fjreflect: " << comp.class_name
                      << " (" << comp.fields.size() << " properties)\n";
            all_components.push_back(std::move(comp));
        }
    }

    if (had_errors) {
        std::cerr << "fjreflect: aborting due to errors\n";
        return 1;
    }

    if (all_components.empty()) {
        std::cout << "fjreflect: no components with [[fj::property]] found\n";
    }

    fjreflect::GenerateOptions opts;
    opts.output_dir = output_dir;
    fjreflect::generate(all_components, opts);

    std::cout << "fjreflect: generated " << all_components.size()
              << " component(s) in " << output_dir << "/\n";
    return 0;
}
