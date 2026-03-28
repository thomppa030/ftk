#include "renderer/frame_graph.hpp"
#include "renderer/thread_command_pools.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <latch>

namespace fjell {

uint32_t FrameGraph::register_image(VkImage image, VkImageAspectFlags aspect,
                                     uint32_t base_layer, uint32_t layer_count) {
    uint32_t id = static_cast<uint32_t>(images_.size());
    TrackedImage img{};
    img.image = image;
    img.aspect = aspect;
    img.base_layer = base_layer;
    img.layer_count = layer_count;
    images_.push_back(img);
    return id;
}

void FrameGraph::begin_frame() {
    passes_.clear();
    for (auto& img : images_) {
        img.current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        img.last_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        img.last_access = 0;
    }
}

void FrameGraph::add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                           std::initializer_list<std::pair<uint32_t, ImageUsage>> uses,
                           uint32_t parallel_group) {
    PassDecl pass;
    pass.name = name;
    pass.execute = std::move(execute);
    pass.image_uses = uses;
    pass.parallel_group = parallel_group;
    passes_.push_back(std::move(pass));
}

VkImageLayout FrameGraph::layout_for(ImageUsage usage, VkImageAspectFlags aspect) {
    switch (usage) {
        case ImageUsage::color_attachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageUsage::depth_attachment:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case ImageUsage::depth_attachment_read:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case ImageUsage::shader_read:
        case ImageUsage::compute_read:
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageUsage::compute_write:
            return VK_IMAGE_LAYOUT_GENERAL;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkPipelineStageFlags2 FrameGraph::stage_for(ImageUsage usage) {
    switch (usage) {
        case ImageUsage::color_attachment:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case ImageUsage::depth_attachment:
        case ImageUsage::depth_attachment_read:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case ImageUsage::shader_read:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case ImageUsage::compute_read:
        case ImageUsage::compute_write:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

VkAccessFlags2 FrameGraph::access_for(ImageUsage usage) {
    switch (usage) {
        case ImageUsage::color_attachment:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case ImageUsage::depth_attachment:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case ImageUsage::depth_attachment_read:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case ImageUsage::shader_read:
        case ImageUsage::compute_read:
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case ImageUsage::compute_write:
            return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    return 0;
}

void FrameGraph::insert_barrier(VkCommandBuffer cmd, TrackedImage& img,
                                 VkImageLayout new_layout,
                                 VkPipelineStageFlags2 dst_stage,
                                 VkAccessFlags2 dst_access) {
    if (img.current_layout == new_layout &&
        (img.last_access & dst_access) == dst_access) {
        return; // already in the right state
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = img.last_stage;
    barrier.srcAccessMask = img.last_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = img.current_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange.aspectMask = img.aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = img.base_layer;
    barrier.subresourceRange.layerCount = img.layer_count;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);

    img.current_layout = new_layout;
    img.last_stage = dst_stage;
    img.last_access = dst_access;
}

void FrameGraph::emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass) {
    for (const auto& [img_id, usage] : pass.image_uses) {
        auto& img = images_[img_id];
        auto needed_layout = layout_for(usage, img.aspect);
        auto needed_stage = stage_for(usage);
        auto needed_access = access_for(usage);
        insert_barrier(cmd, img, needed_layout, needed_stage, needed_access);
    }
}

void FrameGraph::execute(VkCommandBuffer primary, ThreadPool* pool,
                          ThreadCommandPools* cmd_pools, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("frame_graph_execute");
    bool can_parallelize = pool && cmd_pools && pool->thread_count() > 0;

    size_t i = 0;
    while (i < passes_.size()) {
        auto& pass = passes_[i];

        if (pass.parallel_group == 0 || !can_parallelize) {
            // Sequential pass — same as before
            emit_barriers_for_pass(primary, pass);
            pass.execute(primary);
            ++i;
            continue;
        }

        // Collect consecutive passes with the same parallel group
        uint32_t group = pass.parallel_group;
        size_t group_begin = i;
        while (i < passes_.size() && passes_[i].parallel_group == group) {
            ++i;
        }
        size_t group_size = i - group_begin;

        // Emit all barriers for the group on the primary first
        for (size_t p = group_begin; p < group_begin + group_size; ++p) {
            emit_barriers_for_pass(primary, passes_[p]);
        }

        // Record each pass on a secondary command buffer, in parallel
        secondaries_scratch_.resize(group_size);
        auto& secondaries = secondaries_scratch_;
        std::latch done(static_cast<ptrdiff_t>(group_size));

        for (size_t p = 0; p < group_size; ++p) {
            auto thread_idx = static_cast<uint32_t>(p % (pool->thread_count() + 1));
            VkCommandBuffer secondary = cmd_pools->allocate_secondary(thread_idx, frame_index);
            secondaries[p] = secondary;

            auto record = [&passes = passes_, group_begin, p, secondary, &done]() {
                VkCommandBufferInheritanceInfo inheritance{};
                inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

                VkCommandBufferBeginInfo begin_info{};
                begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                begin_info.pInheritanceInfo = &inheritance;

                vkBeginCommandBuffer(secondary, &begin_info);
                passes[group_begin + p].execute(secondary);
                vkEndCommandBuffer(secondary);
                done.count_down();
            };

            // Use the calling thread for the last task, submit the rest to the pool
            if (p < group_size - 1) {
                (void)pool->submit(std::move(record));
            } else {
                record();
            }
        }

        done.wait();

        vkCmdExecuteCommands(primary, static_cast<uint32_t>(group_size), secondaries.data());
    }
}

} // namespace fjell
