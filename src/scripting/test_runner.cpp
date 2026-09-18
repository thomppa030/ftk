#include "scripting/test_runner.hpp"

#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/functional_test.hpp"
#include "scripting/script_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

const char* outcome_label(TestOutcome outcome) {
    switch (outcome) {
        case TestOutcome::passed:    return "PASS";
        case TestOutcome::failed:    return "FAIL";
        case TestOutcome::timed_out: return "TIME";
        case TestOutcome::not_run:   return "SKIP";
    }
    return "????";
}

Result<std::vector<std::string>> discover_test_scenes(const std::string& project_root,
                                                      const std::string& filter) {
    const auto dir = fs::path(project_root) / "Tests";

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return make_error("No Tests directory in the project: " + dir.string());
    }

    std::vector<std::string> scenes;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".fjell") continue;
        if (!filter.empty() &&
            entry.path().stem().string().find(filter) == std::string::npos) {
            continue;
        }
        scenes.push_back(entry.path().string());
    }

    // The order a directory iterator hands back is not defined, and a report
    // whose lines move between runs is one nobody reads twice.
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

namespace {

/// Every access goes through this, so the one unchecked assumption — that an
/// instance whose registered type says is_functional_test really derives from
/// FunctionalTest — lives in a single place.
FunctionalTest* as_test(void* instance) {
    return static_cast<FunctionalTest*>(instance);
}

} // namespace

TestHandle TestHandle::find(Scene& scene, const ScriptManager& scripts) {
    TestHandle handle;
    for (const auto& obj : scene.objects()) {
        for (const auto& comp : obj->scripts) {
            if (!comp.instance) continue;
            const auto* type = scripts.find_script_type(comp.type_name);
            if (type == nullptr || !type->is_functional_test) continue;
            handle.instance_ = comp.instance.get();
            handle.type_name_ = comp.type_name;
            return handle;
        }
    }
    return handle;
}

bool TestHandle::is_ready() const { return as_test(instance_)->is_ready(); }
void TestHandle::start_test() const { as_test(instance_)->start_test(); }
void TestHandle::cleanup_test() const { as_test(instance_)->cleanup_test(); }
bool TestHandle::finished() const { return as_test(instance_)->finished(); }

bool TestHandle::passed() const {
    return as_test(instance_)->test_result() == TestResult::passed;
}

std::string TestHandle::message() const { return as_test(instance_)->test_message(); }

std::vector<std::string> TestHandle::failures() const {
    return as_test(instance_)->failures();
}

float TestHandle::time_limit() const { return as_test(instance_)->time_limit; }

float TestHandle::preparation_time_limit() const {
    return as_test(instance_)->preparation_time_limit;
}

std::string format_report(const std::vector<TestReport>& reports) {
    std::string out;
    int passed = 0;
    int failed = 0;

    for (const auto& r : reports) {
        if (r.outcome == TestOutcome::passed) {
            ++passed;
        } else {
            ++failed;
        }

        char line[256];
        std::snprintf(line, sizeof(line), "%-4s  %-28s %6.2fs",
                      outcome_label(r.outcome), r.name.c_str(),
                      static_cast<double>(r.seconds));
        out += line;
        if (!r.message.empty()) {
            out += "  ";
            out += r.message;
        }
        out += '\n';

        for (const auto& failure : r.failures) {
            out += "        ";
            out += failure;
            out += '\n';
        }
    }

    out += '\n';
    if (reports.empty()) {
        out += "no tests";
    } else if (failed == 0) {
        out += std::to_string(passed) + " passed, " +
               std::to_string(reports.size()) + " total";
    } else {
        out += std::to_string(failed) + " failed, " + std::to_string(passed) +
               " passed, " + std::to_string(reports.size()) + " total";
    }
    out += '\n';
    return out;
}

bool all_passed(const std::vector<TestReport>& reports) {
    return std::all_of(reports.begin(), reports.end(), [](const TestReport& r) {
        return r.outcome == TestOutcome::passed;
    });
}

} // namespace fjell
