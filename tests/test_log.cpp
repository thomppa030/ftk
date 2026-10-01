// The log for the whole test run, set up once before any test: warnings and
// errors only, so a passing run stays quiet and a failing one still says why.

#include "ftk/base/log.hpp"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace {

class LogForTests : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo& /*info*/) override {
        fjell::log::init({.level = spdlog::level::warn, .pattern = "[%T] [%n] [%l] %v"});
    }
};

} // namespace

CATCH_REGISTER_LISTENER(LogForTests)
