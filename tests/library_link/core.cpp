// Links fjell-core alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it calls into each part once.

#include "core/command_history.hpp"
#include "core/log.hpp"
#include "core/math/curve.hpp"
#include "core/thread_pool.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace {

class Count : public fjell::Command {
public:
    explicit Count(int& count) : count_{count} {}
    void execute() override { ++count_; }
    void undo() override { --count_; }
    [[nodiscard]] std::string description() const override { return "Count"; }

private:
    int& count_;
};

} // namespace

int main() {
    fjell::log::init({.level = spdlog::level::warn});

    fjell::ThreadPool pool(1);
    const int answer = pool.submit([] { return 42; }).get();

    int count = 0;
    fjell::CommandHistory history;
    history.execute(std::make_unique<Count>(count));
    history.undo();

    nlohmann::json curve;
    fjell::Curve::linear(0.0f, 1.0f).to_json(curve);

    fjell::log::shutdown();
    const bool ran = answer == 42 && count == 0 && !curve.is_null();
    return ran ? 0 : 1;
}
