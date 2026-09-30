#include "gpu/vulkan/access.hpp"
#include "gpu/transition.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell::gpu;

namespace {

constexpr int ACCESS_COUNT = static_cast<int>(Access::acceleration_trace_compute) + 1;

} // namespace

TEST_CASE("Every access has a layout and stages for what it applies to", "[vulkan][access]") {
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        INFO("access " << i);
        for (bool depth : {false, true}) {
            const vulkan::ImageScope scope = vulkan::image_scope(access, depth);
            CHECK((scope.layout != VK_IMAGE_LAYOUT_UNDEFINED) == applies_to_texture(access));
            CHECK((scope.stages != 0) == applies_to_texture(access));
        }
        // An acceleration structure's memory is a buffer's, ordered alike.
        CHECK((vulkan::buffer_scope(access).stages != 0) ==
              (applies_to_buffer(access) || applies_to_acceleration(access)));
    }
}

TEST_CASE("Every write keeps a bit a barrier makes available", "[vulkan][access]") {
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        if (!access_is_write(access)) continue;
        INFO("access " << i);
        const VkAccessFlags2 image = vulkan::image_scope(access, false).access;
        const VkAccessFlags2 buffer = vulkan::buffer_scope(access).access;
        CHECK(((image | buffer) & vulkan::WRITE_ACCESS_BITS) != 0);
    }
}

TEST_CASE("Acceleration structures are built and traced at their own stages", "[vulkan][access]") {
    const vulkan::BufferScope build = vulkan::buffer_scope(Access::acceleration_build);
    CHECK(build.stages == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    CHECK(build.access == VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    // Vertices, indices and instance records are read as shader reads; the
    // bottom levels a top level names as structures.
    const vulkan::BufferScope input = vulkan::buffer_scope(Access::acceleration_build_input);
    CHECK(input.stages == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    CHECK(input.access ==
          (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR));

    const vulkan::BufferScope trace = vulkan::buffer_scope(Access::acceleration_trace_compute);
    CHECK(trace.stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(trace.access == VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    CHECK(access_is_write(Access::acceleration_build));
    CHECK_FALSE(access_is_read(Access::acceleration_build));
    CHECK_FALSE(access_is_write(Access::acceleration_trace_compute));
    CHECK(applies_to_buffer(Access::acceleration_build_input));
    CHECK_FALSE(applies_to_buffer(Access::acceleration_build));
}

TEST_CASE("A sampled depth image is in the read-only depth layout", "[vulkan][access]") {
    CHECK(vulkan::image_scope(Access::sampled_compute, false).layout ==
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vulkan::image_scope(Access::sampled_compute, true).layout ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

TEST_CASE("Accesses sharing a layout merge their stages", "[vulkan][access]") {
    const auto merged =
        vulkan::image_scope(Access::sampled_fragment | Access::sampled_compute, false);
    REQUIRE(merged.has_value());
    CHECK(merged->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(merged->stages ==
          (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    CHECK(merged->access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

TEST_CASE("Accesses needing different layouts do not merge", "[vulkan][access]") {
    CHECK_FALSE(vulkan::image_scope(Access::sampled_compute | Access::storage_write_compute, false)
                    .has_value());
}

TEST_CASE("No access is an undefined layout that waits on nothing", "[vulkan][access]") {
    const auto none = vulkan::image_scope(AccessSet{}, false);
    REQUIRE(none.has_value());
    CHECK(none->layout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(none->stages == VK_PIPELINE_STAGE_2_NONE);
}

TEST_CASE("A compute queue waits on all commands for graphics-only stages", "[vulkan][access]") {
    CHECK(vulkan::compute_queue_stages(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) ==
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(vulkan::compute_queue_stages(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) ==
          (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
}

TEST_CASE("Accesses in one layout are one state; nothing is no state", "[vulkan][access]") {
    CHECK(same_state(Access::sampled_fragment, Access::sampled_compute, false));
    CHECK_FALSE(same_state(Access::sampled_fragment, Access::storage_write_compute, false));
    CHECK_FALSE(same_state({}, Access::sampled_fragment, false));
    // Sampled depth reads in the read-only depth layout, which a depth test
    // that also samples uses too.
    CHECK(same_state(Access::sampled_fragment, Access::depth_read_sampled, true));
    CHECK_FALSE(same_state(Access::sampled_fragment, Access::depth_read_sampled, false));
}

TEST_CASE("What was made visible to a stage stays visible to it", "[vulkan][access]") {
    const AccessSet both = Access::sampled_fragment | Access::sampled_compute;
    CHECK(texture_already_visible(both, Access::sampled_compute, false, Queue::graphics));
    CHECK_FALSE(
        texture_already_visible(Access::sampled_compute, Access::sampled_fragment, false,
                                Queue::graphics));
    CHECK(buffer_already_visible(Access::storage_buffer_read_compute | Access::indirect_read,
                                 Access::indirect_read, Queue::compute));
    CHECK_FALSE(buffer_already_visible(Access::indirect_read, Access::uniform_read,
                                       Queue::graphics));
}
