#include "ftk/platform/child_process.hpp"
#include "ftk/platform/detail/command_line.hpp"
#include "ftk/platform/detail/program_search.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
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

// The arguments the helper's "args" wrote, each ended by a zero byte.
std::vector<std::string> arguments_written(const std::string& out) {
    std::vector<std::string> written;
    for (std::size_t from = 0; from < out.size();) {
        const std::size_t zero = out.find('\0', from);
        written.push_back(out.substr(from, zero - from));
        from = zero + 1;
    }
    return written;
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
    REQUIRE(arguments_written(read_all(*child)) == args);
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

TEST_CASE("A child gets every letter of its arguments", "[platform][child]") {
    const std::vector<std::string> args{"fj\xc3\xa4rd", "\xe6\x97\xa5\xe6\x9c\xac", "\xce\xa9 and \xc3\xa9"};
    std::vector<std::string> asked{"args"};
    asked.insert(asked.end(), args.begin(), args.end());
    auto child = start_helper(asked);
    REQUIRE(arguments_written(read_all(*child)) == args);
}

TEST_CASE("A program started with a pipe to its output gets its arguments as they were given, every letter",
          "[platform][child]") {
    const std::vector<std::string> args{"plain", "two words", "", "\"quoted\"", "ends in\\", "fj\xc3\xa4rd",
                                        "\xe6\x97\xa5\xe6\x9c\xac"};
    std::vector<std::string> asked{FTK_TEST_CHILD, "lines"};
    asked.insert(asked.end(), args.begin(), args.end());
    auto handle = ftk::platform::spawn_with_pipe(asked);
    REQUIRE(handle.has_value());
    std::vector<std::string> got;
    std::string pending;
    // Read once more after it ends, for what it wrote last.
    for (bool ended = false; !ended;) {
        ended = ftk::platform::poll_exit(*handle);
        for (std::string& line : ftk::platform::read_lines(*handle, pending)) got.push_back(std::move(line));
        if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(handle->exit_code == 0);
    ftk::platform::close_subprocess(*handle);
    REQUIRE(got == args);
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

TEST_CASE("A batch file is told by its name, as Windows reads the name", "[platform][child]") {
    using ftk::platform::detail::is_batch_file;
    for (const std::string_view name : {"run.cmd", "RUN.BAT", "C:\\tools\\code.Cmd", "run.cmd.", "run.bat  "}) {
        INFO(name);
        CHECK(is_batch_file<char>(name));
    }
    for (const std::string_view name : {"run.exe", "cmd", "runcmd", "run.cmdx", "run.cm"}) {
        INFO(name);
        CHECK_FALSE(is_batch_file<char>(name));
    }
}

TEST_CASE("A batch file's arguments are written so cmd.exe acts on nothing in them", "[platform][child]") {
    const auto written = [](std::string_view argument) -> std::optional<std::string> {
        std::string out;
        if (!ftk::platform::detail::append_batch_argument<char>(out, argument)) return std::nullopt;
        return out;
    };
    // A plain word as it is; an empty one, a space or anything cmd acts on
    // in quotes.
    CHECK(written("C:\\dir\\file.txt") == "C:\\dir\\file.txt");
    CHECK(written("") == "\"\"");
    CHECK(written("two words") == "\"two words\"");
    CHECK(written("a&b|c<d>e^f(g)!h") == "\"a&b|c<d>e^f(g)!h\"");
    // A quote doubled, and the backslashes before it or the closing quote.
    CHECK(written("a\"b") == "\"a\"\"b\"");
    CHECK(written("x\\\"y") == "\"x\\\\\"\"y\"");
    CHECK(written("ends\\") == "\"ends\\\\\"");
    // Each % one that starts no name.
    CHECK(written("%PATH%") == "\"%%cd:~,%PATH%%cd:~,%\"");
    // Letters past ASCII in quotes, as they are.
    CHECK(written("fj\xc3\xa4rd") == "\"fj\xc3\xa4rd\"");
    // Nothing carries a line break or a NUL past cmd.
    CHECK_FALSE(written("two\nlines").has_value());
    CHECK_FALSE(written("cr\r").has_value());
    CHECK_FALSE(written(std::string_view("nul\0", 4)).has_value());
}

TEST_CASE("A batch file is run by cmd.exe, the batch file and its arguments in quotes of their own",
          "[platform][child]") {
    using ftk::platform::detail::batch_command_line;
    const std::vector<std::string> args{"--goto", "C:\\game\\my scripts\\coin.cpp:26:3"};
    CHECK(batch_command_line<char>("C:\\tools\\code.cmd", args) ==
          "cmd.exe /e:ON /v:OFF /d /c \"\"C:\\tools\\code.cmd\" --goto \"C:\\game\\my scripts\\coin.cpp:26:3\"\"");
    // What no quoting carries past cmd.
    const std::vector<std::string> none;
    CHECK_FALSE(batch_command_line<char>("C:\\a\"b.cmd", none).has_value());
    CHECK_FALSE(batch_command_line<char>("C:\\tools\\", none).has_value());
    const std::vector<std::string> broken{"two\nlines"};
    CHECK_FALSE(batch_command_line<char>("C:\\tools\\code.cmd", broken).has_value());
}

#ifdef _WIN32
TEST_CASE("A batch file runs with its arguments as they were given, cmd acting on none of them", "[platform][child]") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("ftk-batch-" + std::to_string(ftk::platform::process_id()));
    std::filesystem::create_directories(folder);
    // Hands all it is given on to the helper.
    const std::filesystem::path relay = folder / "relay.cmd";
    std::ofstream(relay, std::ios::binary)
        << "@\"" << std::filesystem::path(FTK_TEST_CHILD).make_preferred().string() << "\" %*\r\n";

    const std::vector<std::string> args{"plain", "two words", "", "\"quoted\"", "ends in\\", "a&b|c<d>e^f",
                                        "(paren)", "!bang!", "%PATH%", "100% sure", "%%cd:~,%", "fj\xc3\xa4rd"};
    std::vector<std::string> asked{relay.string(), "args"};
    asked.insert(asked.end(), args.begin(), args.end());
    {
        auto child = ChildProcess::start(asked);
        REQUIRE(child.has_value());
        CHECK(arguments_written(read_all(**child)) == args);
        CHECK((*child)->wait() == 0);
    }
    // A line break no quoting carries past cmd: refused, never run.
    const std::vector<std::string> broken{relay.string(), "args", "two\nlines"};
    CHECK_FALSE(ChildProcess::start(broken).has_value());

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
#endif

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

TEST_CASE("Windows finds a program under the extensions PATHEXT lists of a file it starts", "[platform][child]") {
    using ftk::platform::detail::program_extensions;
    const std::vector<std::string> windows_own{".com", ".exe", ".bat", ".cmd"};
    CHECK(program_extensions<char>("") == windows_own);
    CHECK(program_extensions<char>(".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC") == windows_own);
    // In the list's order, once each, whatever case it writes them in.
    CHECK(program_extensions<char>(";.Cmd;;.EXE;.cmd;.PY") == std::vector<std::string>{".cmd", ".exe"});
    CHECK(program_extensions<char>(".VBS;.JS") == windows_own);
}

TEST_CASE("A program's name with one of those extensions is itself, else each is added", "[platform][child]") {
    using ftk::platform::detail::program_names;
    const std::vector<std::string> extensions{".com", ".exe", ".bat", ".cmd"};
    CHECK(program_names<char>("code", extensions) ==
          std::vector<std::string>{"code.com", "code.exe", "code.bat", "code.cmd"});
    CHECK(program_names<char>("Code.CMD", extensions) == std::vector<std::string>{"Code.CMD"});
    CHECK(program_names<char>("python3.11", extensions) ==
          std::vector<std::string>{"python3.11.com", "python3.11.exe", "python3.11.bat", "python3.11.cmd"});
    CHECK(program_names<char>(".exe", extensions) ==
          std::vector<std::string>{".exe.com", ".exe.exe", ".exe.bat", ".exe.cmd"});
    CHECK(program_names<char>("", extensions).empty());
}

TEST_CASE("PATH's folders are read as Windows reads them", "[platform][child]") {
    CHECK(ftk::platform::detail::semicolon_list<char>("C:\\a;;\"C:\\Program Files\\b\";C:\\c\\;") ==
          std::vector<std::string>{"C:\\a", "C:\\Program Files\\b", "C:\\c\\"});
    CHECK(ftk::platform::detail::semicolon_list<char>("").empty());
}

#ifdef _WIN32
TEST_CASE("A batch file is found and started by its name alone, as cmd finds it", "[platform][child]") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("ftk-batch-name-" + std::to_string(ftk::platform::process_id()));
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "relay.cmd", std::ios::binary)
        << "@\"" << std::filesystem::path(FTK_TEST_CHILD).make_preferred().string() << "\" %*\r\n";

    const std::filesystem::path named = folder / "relay";
    REQUIRE(ftk::platform::find_program(named.string()) == folder / "relay.cmd");
    const std::vector<std::string> asked{named.string(), "args", "two words"};
    {
        auto child = ChildProcess::start(asked);
        REQUIRE(child.has_value());
        CHECK(arguments_written(read_all(**child)) == std::vector<std::string>{"two words"});
        CHECK((*child)->wait() == 0);
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
#endif

TEST_CASE("A command starting with its program's path in quotes runs as written", "[platform][command]") {
    // As a path with a space needs it, an argument quoted after it too.
    std::string output;
    const std::string command = "\"" + std::string(FTK_TEST_CHILD) + "\" \"folder\"";
    REQUIRE(ftk::platform::run_command(command, output) == 0);
    REQUIRE(output == std::filesystem::current_path().string());
}
