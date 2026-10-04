// A program ftk's tests run as a child (test_child_process.cpp). Its first
// argument says what it does:
//
//     echo        copies its input to its output until the input ends
//     errors      writes "fine" to its output and "oops" to its errors
//     exit <n>    ends with the exit code n
//     folder      writes the folder it runs in
//     args ...    writes each argument after "args", a zero byte after each
//     wait        reads its input until it ends
//     stdio       copies its input to its output through the platform's own
//                 standard streams, byte for byte
//     log         logs "logged" with its log on standard error, then writes
//                 "said" to its output
//
// It writes no line breaks, which Windows would change on the way out
// through the C library; stdio's bytes pass as they are.

#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

int main(int argc, char** argv) {
    const std::string_view does = argc > 1 ? argv[1] : "";
    if (does == "echo") {
        for (int c = std::getchar(); c != EOF; c = std::getchar()) std::putchar(c);
    } else if (does == "errors") {
        std::fputs("fine", stdout);
        std::fputs("oops", stderr);
    } else if (does == "exit" && argc > 2) {
        return std::atoi(argv[2]);
    } else if (does == "folder") {
        std::fputs(std::filesystem::current_path().string().c_str(), stdout);
    } else if (does == "args") {
        for (int i = 2; i < argc; ++i) std::fwrite(argv[i], 1, std::strlen(argv[i]) + 1, stdout);
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
