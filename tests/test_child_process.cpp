#include "ftk/platform/child_process.hpp"
#include "ftk/platform/detail/command_line.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

using ftk::platform::ChildErrors;
using ftk::platform::ChildOptions;
using ftk::platform::ChildProcess;

namespace {

// The helper the tests run (child_helper.cpp), doing what `args` say.
std::unique_ptr<ChildProcess> start_helper(std::vector<std::string> args, const ChildOptions& options = {}) {
    args.insert(args.begin(), FTK_TEST_CHILD);
    auto child = ChildProcess::start(args, options);
    REQUIRE(child.has_value());
    return std::move(*child);
}

// All a child writes to its output, or its errors, until it ends them.
std::string read_all(ChildProcess& child, bool errors = false) {
    std::string out;
    std::array<char, 256> buffer{};
    for (;;) {
        const auto got = errors ? child.read_errors(buffer) : child.read(buffer);
        REQUIRE(got.has_value());
        if (*got == 0) return out;
        out.append(buffer.data(), *got);
    }
}

} // namespace

TEST_CASE("A child reads what it is given and writes back, and its output ends when it does", "[platform][child]") {
    auto child = start_helper({"echo"});
    REQUIRE(child->write("hello world").has_value());
    child->close_input();
    REQUIRE(read_all(*child) == "hello world");
    REQUIRE(child->wait() == 0);
    REQUIRE(child->exit_code() == 0);
}

TEST_CASE("A child's exit code comes back", "[platform][child]") {
    auto child = start_helper({"exit", "3"});
    REQUIRE(child->wait() == 3);
}

TEST_CASE("A child gets its arguments as they were given", "[platform][child]") {
    const std::vector<std::string> args{"plain", "two words", "", "\"quoted\"", "back\\slash", "ends in\\", "a\\\"b"};
    std::vector<std::string> asked{"args"};
    asked.insert(asked.end(), args.begin(), args.end());
    auto child = start_helper(asked);
    const std::string out = read_all(*child);
    std::vector<std::string> got;
    for (std::size_t from = 0; from < out.size();) {
        const std::size_t zero = out.find('\0', from);
        got.push_back(out.substr(from, zero - from));
        from = zero + 1;
    }
    REQUIRE(got == args);
}

TEST_CASE("A child starts in the folder it is given", "[platform][child]") {
    std::random_device random;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("ftk-child-" + std::to_string(random()));
    std::filesystem::create_directories(folder);
    auto child = start_helper({"folder"}, {.folder = folder});
    const std::string out = read_all(*child);
    REQUIRE(std::filesystem::equivalent(out, folder));
    child.reset();
    std::filesystem::remove_all(folder);
}

TEST_CASE("A child's errors are kept to be read, or let go", "[platform][child]") {
    auto kept = start_helper({"errors"}, {.errors = ChildErrors::Kept});
    REQUIRE(read_all(*kept) == "fine");
    REQUIRE(read_all(*kept, true) == "oops");

    auto discarded = start_helper({"errors"});
    REQUIRE(read_all(*discarded) == "fine");
    REQUIRE(read_all(*discarded, true).empty());
}

TEST_CASE("A program that isn't there doesn't start", "[platform][child]") {
    const std::vector<std::string> args{"ftk-no-such-program-anywhere"};
    REQUIRE_FALSE(ChildProcess::start(args).has_value());
}

TEST_CASE("Killing a child ends it, and a read waiting on it finds the end", "[platform][child]") {
    auto child = start_helper({"wait"});
    std::size_t got = 1;
    std::thread reader([&] {
        std::array<char, 16> buffer{};
        got = child->read(buffer).value_or(1);
    });
    REQUIRE_FALSE(child->exit_code().has_value());
    child->kill();
    reader.join();
    REQUIRE(got == 0);
    REQUIRE(child->wait() == -1);
}

TEST_CASE("Writing to a child that has ended is an error, never the end of this program", "[platform][child]") {
    auto child = start_helper({"exit", "0"});
    REQUIRE(child->wait() == 0);
    REQUIRE_FALSE(child->write(std::string(100'000, 'x')).has_value());
}

TEST_CASE("A command line reads back on Windows as the arguments it was made from", "[platform][child]") {
    const std::vector<std::string> args{"prog", "a b", "", "x\"y", "c:\\dir\\", "ends in\\", "a\\\\\"b"};
    REQUIRE(ftk::platform::detail::command_line<char>(args) ==
            "prog \"a b\" \"\" \"x\\\"y\" c:\\dir\\ \"ends in\\\\\" \"a\\\\\\\\\\\"b\"");
}

TEST_CASE("A program's log can keep to standard error, leaving its output to the program", "[platform][child][log]") {
    auto child = start_helper({"log"}, {.errors = ChildErrors::Kept});
    REQUIRE(read_all(*child) == "said");
    REQUIRE(read_all(*child, true).find("logged") != std::string::npos);
    REQUIRE(child->wait() == 0);
}

TEST_CASE("A program's own standard streams pass bytes as they are, line breaks and all", "[platform][child]") {
    auto child = start_helper({"stdio"});
    const std::string bytes = "Content-Length: 2\r\n\r\n{}\n\x01\xff";
    REQUIRE(child->write(bytes).has_value());
    child->close_input();
    REQUIRE(read_all(*child) == bytes);
    REQUIRE(child->wait() == 0);
}

TEST_CASE("A program is found where starting it would find it", "[platform][child]") {
    const std::filesystem::path helper = FTK_TEST_CHILD;
    REQUIRE(ftk::platform::find_program(helper.string()) == helper);
    REQUIRE_FALSE(ftk::platform::find_program(helper.string() + "-gone").has_value());
    REQUIRE_FALSE(ftk::platform::find_program("ftk-no-such-program-anywhere").has_value());
    REQUIRE_FALSE(ftk::platform::find_program("").has_value());
    // A name alone, along the PATH, as every system has one.
#ifdef _WIN32
    REQUIRE(ftk::platform::find_program("cmd").has_value());
#else
    REQUIRE(ftk::platform::find_program("sh").has_value());
#endif
}

TEST_CASE("A command starting with its program's path in quotes runs as written", "[platform][command]") {
    // As a path with a space needs it, an argument quoted after it too.
    std::string output;
    const std::string command = "\"" + std::string(FTK_TEST_CHILD) + "\" \"folder\"";
    REQUIRE(ftk::platform::run_command(command, output) == 0);
    REQUIRE(output == std::filesystem::current_path().string());
}
