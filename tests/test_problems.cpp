#include "ftk/base/problems.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

using namespace ftk;

namespace {

Problem error(std::string message, std::string file = "sea.fjsl", int line = 0) {
    return {.severity = ProblemSeverity::Error, .message = std::move(message), .file = std::move(file), .line = line};
}

Problem warning(std::string message, std::string file = "hud.fjss", int line = 0) {
    return {.severity = ProblemSeverity::Warning, .message = std::move(message), .file = std::move(file), .line = line};
}

std::vector<std::string> messages(const ProblemList& list) {
    std::vector<std::string> out;
    for (const ListedProblem& listed : list.listed()) out.push_back(listed.problem.message);
    return out;
}

} // namespace

TEST_CASE("A source's report replaces what it reported before", "[problems]") {
    ProblemList list;
    list.report("shader:sea", {error("undeclared identifier", "sea.fjsl", 41), error("wrong operand types", "sea.fjsl", 58)});
    list.report("sheet:hud", {warning("Unknown property 'text-shadow'")});
    REQUIRE(list.counts().errors == 2);
    REQUIRE(list.counts().warnings == 1);

    // Fixed one of the two: the source looks again and finds one.
    list.report("shader:sea", {error("wrong operand types", "sea.fjsl", 58)});
    REQUIRE(list.counts().errors == 1);
    REQUIRE(messages(list) == std::vector<std::string>{"wrong operand types", "Unknown property 'text-shadow'"});

    // Fixed both: an empty set takes the source's problems back.
    list.report("shader:sea", {});
    REQUIRE(list.counts().errors == 0);
    REQUIRE(list.counts().total() == 1);

    list.clear("sheet:hud");
    REQUIRE(list.listed().empty());
    REQUIRE_FALSE(list.newest());
}

TEST_CASE("Errors come first, the newest first within each severity", "[problems]") {
    ProblemList list;
    list.report("a", {warning("old warning")});
    list.report("b", {error("old error")});
    list.report("c", {warning("new warning")});
    list.report("d", {error("new error")});

    REQUIRE(messages(list) == std::vector<std::string>{"new error", "old error", "new warning", "old warning"});
}

TEST_CASE("Problems reported together keep the order they were given", "[problems]") {
    ProblemList list;
    list.report("shader:sea", {error("line 41", "sea.fjsl", 41), error("line 58", "sea.fjsl", 58), error("line 90", "sea.fjsl", 90)});

    REQUIRE(messages(list) == std::vector<std::string>{"line 41", "line 58", "line 90"});
    REQUIRE(list.newest()->problem.message == "line 41");
}

TEST_CASE("A problem reported again unchanged keeps its place", "[problems]") {
    ProblemList list;
    list.report("shader:sea", {error("undeclared identifier", "sea.fjsl", 41)});
    list.report("font:title", {error("Font didn't bake")});
    REQUIRE(list.newest()->problem.message == "Font didn't bake");

    // The shader compiles again with the same error and a new one: the old
    // one stays older than the font's, the new one is the newest.
    list.report("shader:sea", {error("undeclared identifier", "sea.fjsl", 41), error("wrong operand types", "sea.fjsl", 58)});
    REQUIRE(messages(list) == std::vector<std::string>{"wrong operand types", "Font didn't bake", "undeclared identifier"});
    REQUIRE(list.newest()->problem.message == "wrong operand types");
}

TEST_CASE("The same problem twice in one set stays twice", "[problems]") {
    ProblemList list;
    list.report("sheet:hud", {warning("Unknown property"), warning("Unknown property")});
    REQUIRE(list.counts().warnings == 2);

    list.report("sheet:hud", {warning("Unknown property"), warning("Unknown property"), warning("Unknown property")});
    REQUIRE(list.counts().warnings == 3);
}

TEST_CASE("The revision moves only when the list changes", "[problems]") {
    ProblemList list;
    const uint64_t empty = list.revision();

    list.clear("nothing reported");
    list.report("nothing reported", {});
    REQUIRE(list.revision() == empty);

    list.report("shader:sea", {error("undeclared identifier")});
    const uint64_t one = list.revision();
    REQUIRE(one != empty);

    list.report("shader:sea", {error("undeclared identifier")});
    REQUIRE(list.revision() == one);

    list.report("shader:sea", {error("undeclared identifier"), error("wrong operand types")});
    REQUIRE(list.revision() != one);
}

TEST_CASE("The newest problem is the one that arrived last, whatever its severity", "[problems]") {
    ProblemList list;
    list.report("shader:sea", {error("undeclared identifier")});
    list.report("sheet:hud", {warning("Unknown property")});

    const auto newest = list.newest();
    REQUIRE(newest);
    REQUIRE(newest->problem.message == "Unknown property");
    REQUIRE(newest->source == "sheet:hud");

    // Once it is fixed, the one before it is the newest again.
    list.clear("sheet:hud");
    REQUIRE(list.newest()->problem.message == "undeclared identifier");
}

TEST_CASE("clear_all takes everything back", "[problems]") {
    ProblemList list;
    list.report("a", {error("one")});
    list.report("b", {warning("two")});
    const uint64_t before = list.revision();

    list.clear_all();
    REQUIRE(list.counts().total() == 0);
    REQUIRE(list.revision() != before);
}

TEST_CASE("Sources report from several threads at once", "[problems]") {
    ProblemList list;
    constexpr int THREADS = 8;
    constexpr int REPORTS = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&list, t] {
            const std::string source = "source " + std::to_string(t);
            for (int i = 0; i < REPORTS; ++i) {
                list.report(source, {error("error " + std::to_string(i)), warning("warning")});
                (void)list.listed();
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    REQUIRE(list.counts().errors == THREADS);
    REQUIRE(list.counts().warnings == THREADS);
}

TEST_CASE("Where a problem is reads as its file's name, line and column", "[problems]") {
    Problem problem{.message = "Unknown property", .file = "/project/ui/hud.fjss", .line = 12, .column = 5};
    CHECK(short_where(problem) == "hud.fjss:12:5");
    CHECK(full_where(problem) == "/project/ui/hud.fjss:12:5");

    problem.column = 0;
    CHECK(short_where(problem) == "hud.fjss:12");
    // A column without a line says nothing.
    problem.line = 0;
    problem.column = 5;
    CHECK(short_where(problem) == "hud.fjss");
    problem.file.clear();
    CHECK(short_where(problem).empty());
    CHECK(full_where(problem).empty());
}

TEST_CASE("clear_prefix takes back the sources whose keys start with it", "[problems]") {
    ProblemList list;
    list.report("sheet:/game/ui/hud.fjss", {warning("one")});
    list.report("sheet:/game/ui/menus/main.fjss", {warning("two")});
    list.report("sheet:/game/uikit.fjss", {warning("three")});
    list.report("shader:/game/ui/water.fjsl", {error("four")});

    const uint64_t before = list.revision();
    list.clear_prefix("sheet:/game/ui/");
    CHECK(messages(list) == std::vector<std::string>{"four", "three"});
    CHECK(list.revision() != before);

    const uint64_t after = list.revision();
    list.clear_prefix("sheet:/nowhere/");
    CHECK(list.revision() == after);
}
