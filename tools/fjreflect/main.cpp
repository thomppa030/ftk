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
              << "  --register <file>     Also write the game module's register_all.cpp\n"
              << "  --help                Show this message\n";
}

int main(int argc, char* argv[]) {
    std::string output_dir = "generated";
    std::string register_path;
    std::vector<std::string> source_files;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--register") == 0 && i + 1 < argc) {
            register_path = argv[++i];
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
    std::vector<fjreflect::ClassDecl> all_classes;
    std::vector<fjreflect::PipeInfo> all_pipes;
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
        for (auto& decl : result.classes) all_classes.push_back(std::move(decl));
        for (auto& pipe : result.pipes) all_pipes.push_back(std::move(pipe));
    }

    // The structs components hold can be declared in any header, so they are
    // resolved once every header has been read.
    fjreflect::LinkResult linked;
    if (!had_errors) {
        linked = fjreflect::link(all_components, all_classes, all_pipes);
        for (const auto& w : linked.warnings) std::cerr << w << "\n";
        for (const auto& e : linked.errors) {
            std::cerr << e << "\n";
            had_errors = true;
        }
    }

    if (had_errors) {
        std::cerr << "fjreflect: aborting due to errors\n";
        return 1;
    }

    if (all_components.empty()) {
        std::cout << "fjreflect: no components with [[fj::property]] found\n";
    }

    const auto files = fjreflect::generate(all_components, linked.structs, all_pipes);
    fjreflect::WriteReport report;
    if (!fjreflect::write_outputs(output_dir, files, report)) {
        std::cerr << "fjreflect: aborting: the output directory could not be updated\n";
        return 1;
    }
    if (!register_path.empty() &&
        fjreflect::write_if_changed(register_path,
                                    fjreflect::register_file(all_components, linked.structs, all_pipes)) ==
            fjreflect::WriteResult::failed) {
        return 1;
    }

    std::cout << "fjreflect: generated " << all_components.size() << " component(s)";
    if (!linked.structs.empty()) std::cout << ", " << linked.structs.size() << " struct(s)";
    if (!all_pipes.empty()) std::cout << ", " << all_pipes.size() << " pipe(s)";
    std::cout << " in " << output_dir << "/ (" << report.written.size() << " of "
              << files.size() << " files changed";
    if (!report.removed.empty()) std::cout << ", " << report.removed.size() << " stale removed";
    std::cout << ")\n";
    return 0;
}
