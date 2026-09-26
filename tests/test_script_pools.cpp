#include "scene/scene.hpp"
#include "scripting/fjell/component.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace fjell;

namespace {

int g_alive = 0;

struct Probe : ScriptComponent {
    int value{0};
    Probe() { ++g_alive; }
    ~Probe() override { --g_alive; }
};

// The scripts of type Probe the scene's pool walks, by node id.
std::vector<uint32_t> walk(Scene& scene) {
    std::vector<uint32_t> ids;
    scene.each_component(component_type_id<Probe>(),
                         [&](Node& node, void*) { ids.push_back(node.id); });
    return ids;
}

uint32_t with_probe(Scene& scene, int value) {
    const uint32_t id = scene.add_node(std::make_unique<Node3D>());
    Node& node = *scene.find(id);
    ScriptEntry entry;
    entry.type_name = "Probe";
    entry.instance = ScriptInstance::make<Probe>(node);
    static_cast<Probe&>(*entry.instance).value = value;
    node.scripts.push_back(std::move(entry));
    return id;
}

/// Records what the scene hands it, and saves and ends scripts the way the
/// script manager does.
struct RecordingHost final : ScriptHost {
    std::vector<uint32_t> left;
    std::vector<uint32_t> returned;
    std::vector<int> saved;

    void scripts_leaving(Node& node) override {
        left.push_back(node.id);
        for (auto& entry : node.scripts) {
            if (entry.instance) saved.push_back(static_cast<Probe&>(*entry.instance).value);
            entry.instance.reset();
        }
    }
    void scripts_returned(Node& node) override {
        returned.push_back(node.id);
        for (auto& entry : node.scripts) entry.instance = ScriptInstance::make<Probe>(node);
    }
};

} // namespace

TEST_CASE("a script lives in its type's pool until it is let go", "[scripts][pool]") {
    g_alive = 0;
    {
        Scene scene;
        const uint32_t a = with_probe(scene, 1);
        const uint32_t b = with_probe(scene, 2);
        CHECK(walk(scene) == std::vector{a, b});
        CHECK(g_alive == 2);

        scene.find(a)->scripts.front().instance.reset();
        CHECK(walk(scene) == std::vector{b});
        CHECK(g_alive == 1);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a script keeps its address while others come and go", "[scripts][pool]") {
    Scene scene;
    const uint32_t kept = with_probe(scene, 7);
    const ScriptComponent* address = scene.find(kept)->scripts.front().instance.get();
    for (int i = 0; i < 500; ++i) {
        const uint32_t other = with_probe(scene, i);
        scene.remove(other);
    }
    CHECK(scene.find(kept)->scripts.front().instance.get() == address);
    CHECK(static_cast<const Probe*>(address)->value == 7);
}

TEST_CASE("a node in no scene has no live script", "[scripts][pool]") {
    g_alive = 0;
    Node3D loose;
    CHECK_FALSE(ScriptInstance::make<Probe>(loose));
    CHECK(g_alive == 0);
}

TEST_CASE("a node taken out of the scene hands its scripts to the host and gets them back",
          "[scripts][pool]") {
    g_alive = 0;
    {
        Scene scene;
        RecordingHost host;
        scene.set_script_host(&host);
        const uint32_t id = with_probe(scene, 5);

        std::unique_ptr<Node> out = scene.extract(id);
        CHECK(host.left == std::vector{id});
        CHECK(host.saved == std::vector{5});
        CHECK_FALSE(out->scripts.front().instance);
        CHECK(walk(scene).empty());
        CHECK(g_alive == 0);

        scene.restore(std::move(out), 0);
        CHECK(host.returned == std::vector{id});
        CHECK(walk(scene) == std::vector{id});
        CHECK(g_alive == 1);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("without a host, a node taken out still lets its scripts go", "[scripts][pool]") {
    g_alive = 0;
    Scene scene;
    const uint32_t id = with_probe(scene, 3);
    std::unique_ptr<Node> out = scene.extract(id);
    CHECK_FALSE(out->scripts.front().instance);
    CHECK(walk(scene).empty());
    CHECK(g_alive == 0);
}
