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
              << "  --help                Show this message\n";
}

int main(int argc, char* argv[]) {
    std::string output_dir = "generated";
    std::vector<std::string> source_files;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
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

    const auto files = fjreflect::generate(all_components);
    fjreflect::WriteReport report;
    if (!fjreflect::write_outputs(output_dir, files, report)) {
        std::cerr << "fjreflect: aborting: the output directory could not be updated\n";
        return 1;
    }

    std::cout << "fjreflect: generated " << all_components.size()
              << " component(s) in " << output_dir << "/ (" << report.written.size() << " of "
              << files.size() << " files changed";
    if (!report.removed.empty()) std::cout << ", " << report.removed.size() << " stale removed";
    std::cout << ")\n";
    return 0;
}
