// A program ftk's tests run as a child (test_child_process.cpp). Its first
// argument says what it does:
//
//     echo        copies its input to its output until the input ends
//     errors      writes "fine" to its output and "oops" to its errors
//     exit <n>    ends with the exit code n
//     folder      writes the folder it runs in
//     args ...    writes each argument after "args", a zero byte after each
//     lines ...   writes each argument after "lines" on a line of its own,
//                 through the platform's own standard output
//     wait        reads its input until it ends
//     stdio       copies its input to its output through the platform's own
//                 standard streams, byte for byte
//     log         logs "logged" with its log on standard error, then writes
//                 "said" to its output
//
// Its arguments are UTF-8 on every system. It writes no line breaks through
// the C library, which Windows would change on the way out; the platform's
// own standard streams pass bytes as they are.

#include "ftk/base/log.hpp"
#include "ftk/base/utf8.hpp"
#include "ftk/platform/platform.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Does what `args[0]` says, with the rest of `args` as its arguments.
int run(const std::vector<std::string>& args) {
    const std::string_view does = args.empty() ? std::string_view{} : std::string_view(args[0]);
    if (does == "echo") {
        for (int c = std::getchar(); c != EOF; c = std::getchar()) std::putchar(c);
    } else if (does == "errors") {
        std::fputs("fine", stdout);
        std::fputs("oops", stderr);
    } else if (does == "exit" && args.size() > 1) {
        return std::atoi(args[1].c_str());
    } else if (does == "folder") {
        std::fputs(std::filesystem::current_path().string().c_str(), stdout);
    } else if (does == "args") {
        for (std::size_t i = 1; i < args.size(); ++i) std::fwrite(args[i].c_str(), 1, args[i].size() + 1, stdout);
    } else if (does == "lines") {
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (!ftk::platform::write_standard_output(args[i] + "\n")) return 3;
        }
    } else if (does == "stdio") {
        std::array<char, 64> buffer{};
        for (;;) {
            const auto got = ftk::platform::read_standard_input(buffer);
            if (!got || *got == 0) break;
            if (!ftk::platform::write_standard_output(std::string_view(buffer.data(), *got))) return 3;
        }
    } else if (does == "log") {
        ftk::log::init({.level = spdlog::level::warn, .pattern = "%v", .to_standard_error = true});
        FTK_CORE_WARN("logged");
        ftk::log::shutdown();
        if (!ftk::platform::write_standard_output("said")) return 3;
    } else if (does == "wait") {
        while (std::getchar() != EOF) {
        }
    } else {
        return 2;
    }
    return 0;
}

} // namespace

#ifdef _WIN32
// Windows hands the arguments over as they were given in wmain's UTF-16;
// main's would be in the code page, which has no letter for most of them.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(ftk::utf8::from_path(std::filesystem::path(argv[i])));
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
