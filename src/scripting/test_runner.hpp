#pragma once

#include "ftk/base/result.hpp"
#include "scene/component_type.hpp"
#include "scripting/script_call.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct Node;
class Scene;
class ScriptManager;

/// What one test run came to, in the runner's own terms. Mirrors the SDK's
/// TestResult without depending on it, so the report survives a scene whose
/// scripts failed to load.
enum class TestOutcome : uint8_t {
    passed,
    failed,
    timed_out,
    /// The scene loaded but held no test component, or its scripts are
    /// missing. Reported, and counted as a failure — a test that cannot run
    /// is not a test that passed.
    not_run,
};

[[nodiscard]] const char* outcome_label(TestOutcome outcome);

/// One finished test, as the summary prints it.
struct TestReport {
    std::string name;       ///< The component's type name.
    std::string scene;      ///< Scene file the test was found in.
    TestOutcome outcome{TestOutcome::not_run};
    std::string message;    ///< Why it ended, from finish_test().
    std::vector<std::string> failures;
    float seconds{0.0f};    ///< How long the test ran, excluding preparation.
};

/// Finds the `.fjell` scenes under a project's `Tests/` directory, in a stable
/// order so two runs report the same list. Returns an error when the directory
/// is missing, so a typo'd project does not read as "no tests".
[[nodiscard]] ftk::Result<std::vector<std::string>> discover_test_scenes(
    const std::string& project_root, const std::string& filter = {});

/// Drives one FunctionalTest through its lifecycle. It finds the test again
/// by its node and type at every call rather than holding on to it, so a test
/// whose node is destroyed or whose script is taken off reads as gone()
/// instead of being called into. Only this translation unit needs the SDK
/// header: `find()` accepts a script only when its registered type carries
/// `is_functional_test`, which makes the cast to FunctionalTest sound.
class TestHandle {
public:
    /// The FunctionalTest in a loaded scene, or an empty handle if it has
    /// none. The first one found wins; a fixture holds one test.
    [[nodiscard]] static TestHandle find(Scene& scene, const ScriptManager& scripts);

    /// Whether the scene had a test.
    [[nodiscard]] bool valid() const { return scene_ != nullptr; }
    /// Whether the test was found and is gone since. Every other call on a
    /// gone test does nothing and answers false, empty or 0.
    [[nodiscard]] bool gone() const { return valid() && instance() == nullptr; }
    [[nodiscard]] const std::string& type_name() const { return type_name_; }

    [[nodiscard]] bool is_ready() const;
    void start_test() const;
    void cleanup_test() const;

    /// Whether the test has reported a verdict and should stop being ticked.
    [[nodiscard]] bool finished() const;
    /// The verdict, once `finished()`. `passed` maps to TestOutcome::passed.
    [[nodiscard]] bool passed() const;
    [[nodiscard]] std::string message() const;
    [[nodiscard]] std::vector<std::string> failures() const;

    /// Seconds the test may run, and may spend getting ready. 0 means no
    /// limit.
    [[nodiscard]] float time_limit() const;
    [[nodiscard]] float preparation_time_limit() const;

private:
    /// The test's script, or null once it is gone.
    [[nodiscard]] void* instance() const;
    /// A call into the test, for call_script().
    [[nodiscard]] ScriptCall call(const char* hook, const void* test) const;

    Scene* scene_{nullptr};
    uint32_t node_id_{0};
    ComponentTypeId type_id_{0};
    std::string type_name_;
};

/// Formats the end-of-run summary, one line per test plus a tally. Separate
/// from the running so it can be tested without an engine.
[[nodiscard]] std::string format_report(const std::vector<TestReport>& reports);

/// Whether every test passed. An empty run passes: a project with no tests is
/// not a failing project.
[[nodiscard]] bool all_passed(const std::vector<TestReport>& reports);

} // namespace fjell
