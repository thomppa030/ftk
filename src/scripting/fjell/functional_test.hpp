// functional_test.hpp — Gameplay test components for Fjell game projects
#pragma once

#include "fjell/component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fjell {

/// How a test ended.
enum class TestResult : uint8_t {
    running,    ///< Not finished yet. Never a reported outcome.
    passed,     ///< Every assertion held and the test finished on its own.
    failed,     ///< An assertion failed, or the test called finish_test().
    timed_out,  ///< The time limit ran out before the test finished.
    skipped,    ///< The test asked not to be run.
};

/// Base class for gameplay tests, run by `fjell <project> --run-tests`.
///
/// A test is a scene plus a component: the scene is the fixture (where the
/// character starts, where the trap sits), and the component is the test. Put
/// one on a Node3D in a scene under the project's `Tests/` directory and the
/// runner will find it.
///
/// The runner loads each test's scene, starts play, waits for `is_ready()`,
/// calls `start_test()`, then ticks until the test reports a result or runs
/// out of time. Tests run one at a time — there is one world to put a fixture
/// in — but a single test spans as many frames as it needs.
///
/// Failing an assertion records it and carries on, so one run reports
/// everything it can see rather than only the first problem. The test ends
/// when it calls `finish_test()` or its `time_limit` expires; a test that
/// never finishes is a failure, not a hang.
///
/// @code
/// class TrapTriggerTest : public fjell::FunctionalTest {
///     FJELL_COMPONENT(TrapTriggerTest)
/// public:
///     // A test's own properties are reflected as any component's are.
///     [[fj::property]] uint32_t trap_id = 0;
///
///     void start_test() override {
///         time_limit = 5.0f;
///         start_pos = transform().position;
///     }
///
///     void tick(float dt) override {
///         transform().position.z += 2.0f * dt;
///     }
///
///     void on_trigger_enter(uint32_t other) override {
///         if (other != trap_id) return;
///         assert_true(transform().position != start_pos, "the character moved");
///         finish_test(fjell::TestResult::passed, "trap fired");
///     }
///
/// private:
///     glm::vec3 start_pos{0.0f};
/// };
/// @endcode
class FunctionalTest : public ScriptComponent {
public:
    /// Polled every frame before the test starts, until it returns true.
    /// Override to wait for an async load or for physics to settle. Taking
    /// longer than `preparation_time_limit` fails the test without running it.
    virtual bool is_ready() { return true; }

    /// Called once, the frame after `is_ready()` first returns true. Set up
    /// what the test needs and record whatever the assertions compare against.
    virtual void start_test() {}

    /// Called once the result is known, however it was reached — including on
    /// a timeout. Release anything the test holds.
    virtual void cleanup_test() {}

    /// Seconds the test may run for before it fails as `timed_out`. 0 removes
    /// the limit, which is rarely what you want: a test that can hang stops
    /// the whole suite.
    ///
    /// Set it in the constructor or `start_test()`. It is a plain field, not a
    /// `[[fj::property]]`: fjreflect only scans a game project's own sources,
    /// so an attribute here would reach neither the inspector nor the scene
    /// file. A test that wants to author its limit per instance declares its
    /// own property and assigns this from it.
    float time_limit = 10.0f;

    /// Seconds `is_ready()` may keep returning false before the test fails.
    /// Authored the same way as `time_limit`.
    float preparation_time_limit = 5.0f;

    // --- Called by the runner, not from a test ---

    /// The result so far. `running` until the test finishes.
    [[nodiscard]] TestResult test_result() const { return result_; }

    /// Why the test ended, as passed to `finish_test()`. Empty if it has not.
    [[nodiscard]] const std::string& test_message() const { return message_; }

    /// Every assertion that failed, in the order they failed. Reported by the
    /// runner; a passing test leaves this empty.
    [[nodiscard]] const std::vector<std::string>& failures() const { return failures_; }

    /// Whether the test has reported a result and should stop being ticked.
    [[nodiscard]] bool finished() const { return result_ != TestResult::running; }

protected:
    /// End the test now. The first call wins, so an assertion failure already
    /// recorded is not overwritten by a later `passed`.
    void finish_test(TestResult result, const char* message = "") {
        if (result_ != TestResult::running) return;
        // A test that failed an assertion cannot pass, whatever it says here.
        result_ = (result == TestResult::passed && !failures_.empty())
                      ? TestResult::failed
                      : result;
        message_ = message != nullptr ? message : "";
    }

    /// Record a failure without ending the test, so one run reports every
    /// problem it can see. The test still finishes as `failed`.
    void add_failure(const std::string& what) {
        failures_.push_back(what);
    }

    /// @return `cond`, so a test can bail out of a check whose precondition
    ///         already failed rather than assert into garbage afterwards.
    bool assert_true(bool cond, const char* what) {
        if (!cond) add_failure(std::string(what) + ": expected true, got false");
        return cond;
    }

    /// @return true when the values match.
    bool assert_false(bool cond, const char* what) {
        if (cond) add_failure(std::string(what) + ": expected false, got true");
        return !cond;
    }

    /// Compare two floats within `tolerance`. Real physics never lands on an
    /// exact value, so every numeric assertion takes one.
    /// @return true when they match.
    bool assert_equal(float actual, float expected, const char* what,
                      float tolerance = 1e-4f) {
        const float diff = actual - expected;
        if (diff > tolerance || diff < -tolerance) {
            add_failure(format_mismatch(what, to_text(actual), to_text(expected)));
            return false;
        }
        return true;
    }

    /// Compare two positions or directions within `tolerance`, per component.
    /// @return true when they match.
    bool assert_equal(const glm::vec3& actual, const glm::vec3& expected,
                      const char* what, float tolerance = 1e-4f) {
        for (int i = 0; i < 3; ++i) {
            const float diff = actual[i] - expected[i];
            if (diff > tolerance || diff < -tolerance) {
                add_failure(format_mismatch(what, to_text(actual), to_text(expected)));
                return false;
            }
        }
        return true;
    }

private:
    static std::string to_text(float v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
        return buf;
    }

    static std::string to_text(const glm::vec3& v) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "(%.4f, %.4f, %.4f)",
                      static_cast<double>(v.x), static_cast<double>(v.y),
                      static_cast<double>(v.z));
        return buf;
    }

    static std::string format_mismatch(const char* what, const std::string& actual,
                                       const std::string& expected) {
        return std::string(what) + ": expected " + expected + ", got " + actual;
    }

    TestResult result_{TestResult::running};
    std::string message_;
    std::vector<std::string> failures_;
};

} // namespace fjell
