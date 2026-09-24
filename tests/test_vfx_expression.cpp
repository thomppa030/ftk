#include "renderer/systems/vfx_expression.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::ExpressionGraph;
using fjell::module_node_id;

TEST_CASE("Removing a module keeps the links into the modules after it", "[vfx]") {
    ExpressionGraph graph;
    graph.add_link(5000050, module_node_id(0, 0) * 100 + 1);   // into the module that goes
    graph.add_link(5000050, module_node_id(0, 2) * 100 + 3);   // into one after it
    graph.add_link(5000050, module_node_id(1, 2) * 100);       // into another stack

    graph.module_removed(0, 0);
    REQUIRE(graph.links.size() == 2);
    CHECK(graph.links[0].target_pin_id == module_node_id(0, 1) * 100 + 3);
    CHECK(graph.links[1].target_pin_id == module_node_id(1, 2) * 100);
}
