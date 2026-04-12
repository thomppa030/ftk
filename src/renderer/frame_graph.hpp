#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fjell {

class ThreadPool;
class ThreadCommandPools;

// How a pass uses an image
enum class ImageUsage : uint8_t {
    color_attachment,       // write as color render target
    depth_attachment,       // write as depth render target
    depth_attachment_read,  // read depth (no write) during rendering
    shader_read,            // sample in a fragment shader
    compute_read,           // sample in a compute shader
    compute_write,          // write as a storage image in a compute shader
    transfer_src,           // source of a copy/blit operation
    transfer_dst,           // destination of a copy/blit operation
};

// Image tracked by the frame graph
struct TrackedImage {
    VkImage image{VK_NULL_HANDLE};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    VkImageLayout current_layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 last_stage{VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT};
    VkAccessFlags2 last_access{0};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
};

// A pass declaration: what images it reads and writes
struct PassDecl {
    std::string name;
    std::function<void(VkCommandBuffer)> execute;
    std::vector<std::pair<uint32_t, ImageUsage>> image_uses; // image_id, usage
    uint32_t parallel_group{0}; // 0 = sequential, >0 = parallel group ID
};

// Lightweight frame graph that tracks image layouts and inserts barriers.
// Not a full dependency graph — pass order is explicit, the graph just
// handles transitions.
class FrameGraph {
public:
    // Register an image to track. Returns an ID.
    uint32_t register_image(VkImage image, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            uint32_t base_layer = 0, uint32_t layer_count = 1);

    // Start a new frame — reset all layouts to UNDEFINED
    void begin_frame();

    // Add a pass that uses images. The execute callback records commands into
    // the provided command buffer (primary for sequential, secondary for parallel).
    void add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                  std::initializer_list<std::pair<uint32_t, ImageUsage>> uses,
                  uint32_t parallel_group = 0);
    void add_pass(const std::string& name, std::function<void(VkCommandBuffer)> execute,
                  std::vector<std::pair<uint32_t, ImageUsage>> uses,
                  uint32_t parallel_group = 0);

    // Execute all passes, inserting barriers between them.
    // Passes with the same parallel_group > 0 are recorded in parallel on
    // secondary command buffers via the thread pool.
    void execute(VkCommandBuffer primary, ThreadPool* pool,
                 ThreadCommandPools* cmd_pools, uint32_t frame_index);

private:
    static VkImageLayout layout_for(ImageUsage usage, VkImageAspectFlags aspect);
    static VkPipelineStageFlags2 stage_for(ImageUsage usage);
    static VkAccessFlags2 access_for(ImageUsage usage);

    void insert_barrier(VkCommandBuffer cmd, TrackedImage& img,
                        VkImageLayout new_layout,
                        VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access);

    void emit_barriers_for_pass(VkCommandBuffer cmd, const PassDecl& pass);

    std::vector<TrackedImage> images_;
    std::vector<PassDecl> passes_;

    // Reusable scratch for parallel group secondary command buffers
    std::vector<VkCommandBuffer> secondaries_scratch_;
};

} // namespace fjell
