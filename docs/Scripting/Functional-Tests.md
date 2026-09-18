# Functional Tests

A functional test is a gameplay scenario the engine plays for you and checks
the outcome of: a character walks into a trap, a door opens when the lever is
pulled, a dropped crate lands on the floor instead of through it. It runs the
real engine on a real GPU, because the things worth testing in a game —
physics, animation, rendering — are the things you cannot fake.

Run the suite from the shell:

```bash
fjell <project> --run-tests
```

## This is for your game, not for the engine

The engine has its own tests: Catch2, under `tests/`, covering logic that needs
no engine and no GPU — parsers, math, serialisation. Those run in a second and
prove a function does what it says.

They cannot reach a character falling through terrain, a play-stop cycle
leaking an emitter, or a UI field that freezes after the first frame. Every one
of those is a real bug this engine has had, and none of them is a unit test's
job. That is the gap this fills.

So: **Catch2 tests the engine, functional tests test a game.** The engine ships
the framework; your project ships the tests.

## The three layers

| Layer | The question it answers | What it is |
|---|---|---|
| **Test** | Did it do the right thing? | A C++ component in your project |
| **Scene** | What is the world made of? | A `.fjell` file, authored in the editor |
| **Scenario** | What state is the world in? | A `.fjscenario`, optional |

The test is C++ because assertions need loops, tolerances and access to
component state — a text format for them would be a programming language badly
reinvented. The scene is the fixture because fixtures are *spatial*: where the
character starts and where the trap sits belong in the editor, not in a file
you hand-write. A [scenario](../Environment/Weather.md) is the ordinary
world-driving script, for a test that needs rain or a fixed hour; most tests
name none.

## Writing one

Put a scene under your project's `Tests/` directory, and a component deriving
from `fjell::FunctionalTest` on a node in it. That is the whole contract — no
manifest, no registration.

```cpp
#include "fjell/functional_test.hpp"
#include "fjell/registry.hpp"

class DoorOpensTest : public fjell::FunctionalTest {
    FJELL_COMPONENT(DoorOpensTest)

    [[fj::property]] float pull_force = 40.0f;

    void start_test() override {
        time_limit = 5.0f;
        start_height_ = transform().position.y;
    }

    void test_tick(float dt) override {
        // drive the world here
    }

    void on_trigger_enter(uint32_t other_id) override {
        assert_true(other_id == expected_id_, "the lever was pulled");
        finish_test(fjell::TestResult::passed, "the door opened");
    }

private:
    float start_height_{0.0f};
    uint32_t expected_id_{0};
};
```

A test is an ordinary script component, so everything a component can do it can
do: `[[fj::property]]` fields show up in the inspector and save with the scene,
`self()` and `transform()` work, and the collision callbacks
(`on_trigger_enter`, `on_collision_enter`) arrive as usual. See
[Writing Components](../Writing-Components.md).

### The lifecycle

A test spans frames — the character has to walk, physics has to tick, the
contact lands a frame or two later. So it is a lifecycle rather than a
function:

| | |
|---|---|
| `is_ready()` | Polled every frame until it returns true. Wait for an async load, or for the character to land. Default: ready at once. |
| `start_test()` | Called once, when the test is ready. Record what the assertions will compare against. |
| `test_tick(dt)` | Called each frame while the test runs. Drive the world here. |
| `cleanup_test()` | Called once the result is known, however it was reached — including a timeout. |

**Override `test_tick()`, not `tick()`.** The script subsystem ticks every
component every frame, including while a test is still waiting on `is_ready()`
and after it has reported its verdict. A test that drove the world in those
windows would be moving things before it started measuring — which is exactly
how the trap test below once reported a shorter walk than it had performed.
`tick()` is `final` on the base and gates `test_tick()` to the running window,
so the mistake does not compile.

### Assertions

```cpp
assert_true(cond, "what this checks");
assert_false(cond, "what this checks");
assert_equal(actual, expected, "what this checks", tolerance);   // float
assert_equal(actual, expected, "what this checks", tolerance);   // vec3
```

Every numeric assertion takes a **tolerance**, defaulting to `1e-4`. Real
physics never lands on an exact value, so exact float equality is always the
wrong question.

A failed assertion **records and carries on** — it does not end the test. One
run then reports everything it can see rather than only the first problem. Each
returns `bool`, so a test can bail out of a check whose precondition already
failed instead of asserting into garbage afterwards:

```cpp
if (!assert_true(door != nullptr, "the door exists")) return;
assert_equal(door->angle, 90.0f, "the door swung fully open", 0.5f);
```

A test ends when it calls `finish_test()` or runs out of time. A test that
failed an assertion **cannot then pass**, whatever it passes to `finish_test`.

### Time limits

`time_limit` (10 s) bounds the run, `preparation_time_limit` (5 s) bounds the
wait on `is_ready()`. They are separate because loading slowly and behaving
wrongly are different failures and should read differently. Set them in
`start_test()`.

A test that never finishes is a **failure at its limit, never a hang** — one
bad test cannot wedge the suite. Setting a limit to 0 removes it, which is
rarely what you want.

They are plain fields rather than `[[fj::property]]`, because `fjreflect` only
scans your project's own sources; an attribute on the engine's base class would
reach neither the inspector nor the scene file. A test that wants to author its
limit declares its own property and assigns from it.

## Running the suite

```bash
fjell <project> --run-tests            # every test
fjell <project> --run-tests Trap       # only scenes whose name contains "Trap"
```

Each scene runs in turn — never two at once, because there is one world to put
a fixture in. The summary names each test, what it came to, and every assertion
that failed with its expected and actual value:

```
FAIL  DoorOpensTest                  2.01s  claims to pass
        the door swung fully open: expected 90.0000, got 4.9700
TIME  ElevatorTest                  10.00s  never finished
PASS  TrapTriggerTest                0.93s  the trap fired

2 failed, 1 passed, 3 total
```

The process exits 0 only if every test passed, so a shell or a CI job can act
on it. A scene under `Tests/` holding no test component is reported rather than
passed over — a test that cannot run is not a test that passed.

**Editor build only.** In a game build (one with `game.fjell` beside the
executable) stopping play closes the window rather than ending the session, so
the first test would end the whole run. The runner says so instead of reporting
a suite that quietly stopped after one test.

## A worked example

The trap test: a character walks forward until a trigger volume notices it.

```cpp
bool TrapTriggerTest::is_ready() {
    // It starts above the floor and is dropped onto it by gravity. Walking
    // before it lands would test a fall, not a walk.
    character_id_ = fjell::SceneAPI::find_node(character.c_str());
    if (character_id_ == 0) return false;

    fjell::Node3D walker{character_id_};
    auto* cc = walker.get_component<fjell::CharacterComponent>();
    if (cc == nullptr) return false;
    return cc->ground_state == fjell::CharacterGroundState::OnGround;
}

void TrapTriggerTest::test_tick(float) {
    fjell::Node3D walker{character_id_};
    auto* cc = walker.get_component<fjell::CharacterComponent>();
    if (cc == nullptr) return;

    // Horizontal velocity does not persist as a decision — write it every
    // tick. Vertical is gravity's, so leave it alone.
    cc->velocity.x = 0.0f;
    cc->velocity.z = walk_speed;
}

void TrapTriggerTest::on_trigger_enter(uint32_t other_id) {
    if (!assert_true(other_id == character_id_,
                     "the trap was entered by the character")) return;

    const auto travelled = fjell::Node3D{character_id_}.position() - start_position_;
    assert_true(travelled.z > 0.0f, "the character walked forward");
    finish_test(fjell::TestResult::passed, "the trap fired");
}
```

The component sits on the **trap**, not on the character, because a trigger
callback goes to the node whose collider was entered.

Note what it does *not* do: it never sets the character's position. Writing
`velocity` and letting the solver move the capsule is what makes this a test of
character movement rather than a test of arithmetic. Driving the world directly
in C++ like this covers movement, physics and collision dispatch; it does not
cover input bindings, which are not yet reachable from a test.

## Things that will bite

**A collider is scaled by its node.** `half_extents` is multiplied by the
node's scale, so a trap you scaled to 2.0 to make it visible has a trigger
twice the size you wrote. Match the two or the test fires early, and silently:
it still passes, just not for the reason you think.

**A passing test is not proof.** Check its duration against the physics. A 2.8
metre walk at 3 m/s takes 0.93 s; a pass in 0.41 s means something fired that
should not have. Run-to-run variance in a scenario that should be deterministic
is itself a defect.

**Subsystems re-derive state from the scene every frame.** Anything a test sets
*beside* a subsystem is overwritten on the next tick. Write the authoring node.

**Integrators start at zero.** Wetness, GI convergence, temporal history and
the sea's wind response all ramp toward their value over seconds. A test that
asserts too early measures the ramp instead of the value — set the integrator
outright, or wait out its time constant.

**A test component must sit on a `Node3D`.** Script components on plain folder
nodes are skipped.

## Not yet

- **Synthetic input** — tests drive the world in C++; they cannot press a key,
  so input bindings are not covered.
- **Screenshot comparison** — no reference-image workflow.
- **An editor panel** — the CLI is the whole interface.
- **CI** — the suite needs a GPU, so it runs where there is one. Exit codes and
  frame-based scheduling are in place for the day there is a runner with one.
