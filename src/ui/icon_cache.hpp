#pragma once

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class IconCache {
public:
    struct IconEntry {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkSampler sampler{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    IconCache(VkDevice device, VkPhysicalDevice physical_device,
              VkCommandPool command_pool, VkQueue queue,
              const std::string& icons_dir);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;
    IconCache(IconCache&&) = delete;
    IconCache& operator=(IconCache&&) = delete;

    /// Returns the ImGui descriptor set for a named icon (e.g. "folder", "mesh").
    /// The descriptor is for the editor's ImGui context (created at load time).
    [[nodiscard]] VkDescriptorSet icon(const std::string& name) const;

    /// Returns an ImGui texture ID for a named icon, registered in the
    /// *current* ImGui context. Use this from secondary windows (file browser,
    /// import dialog) that have their own ImGui context.
    [[nodiscard]] ImTextureID icon_for_current_context(const std::string& name);

    /// Draw a small icon inline (for panel headers). Call right after ImGui::Begin().
    inline void draw_panel_icon(const char* icon_name) const {
        auto desc = icon(icon_name);
        if (!desc) return;
        float size = ImGui::GetTextLineHeight();
        ImGui::Image(reinterpret_cast<ImTextureID>(desc), {size, size});
        ImGui::SameLine();
    }

    [[nodiscard]] size_t count() const { return icons_.size(); }

    /// Get or create a thumbnail for an arbitrary image file.
    /// Returns VK_NULL_HANDLE on failure. Thumbnails are cached by path.
    [[nodiscard]] VkDescriptorSet thumbnail(const std::string& path);

    /// Return cached thumbnail only (no loading). VK_NULL_HANDLE if not yet loaded.
    [[nodiscard]] VkDescriptorSet thumbnail_cached(const std::string& path) const;

    /// Release all cached thumbnails.
    void clear_thumbnails();

    /// Queue async thumbnail decode for a list of image paths.
    /// CPU decode runs on background threads, GPU upload happens in poll_thumbnails().
    void preload_thumbnails(const std::vector<std::string>& paths, class ThreadPool& pool);

    /// Upload completed thumbnail data to GPU. Call once per frame from main thread.
    void poll_thumbnails();

    /// Generate .fjcache thumbnail on disk for an image file (no GPU work).
    /// Safe to call from any thread. Skips if cache is already valid.
    static void ensure_thumbnail_cache(const std::string& path);

    /// Upload RGBA pixel data as a Vulkan image + ImGui descriptor.
    /// Reusable for icons, thumbnails, material previews, etc.
    IconEntry upload_rgba(const uint8_t* pixels, int w, int h);

    /// Global instance — set once at engine init, used by all panels.
    [[nodiscard]] static IconCache* instance() { return s_instance; }
    static void set_instance(IconCache* cache) { s_instance = cache; }

private:
    static inline IconCache* s_instance{nullptr};

    void load_icon(const std::string& name, const std::string& path);

    VkDevice device_;
    VkPhysicalDevice physical_device_;
    VkCommandPool command_pool_;
    VkQueue queue_;

    std::unordered_map<std::string, IconEntry> icons_;
    std::unordered_map<std::string, IconEntry> thumbnails_;

    // Per-context icon descriptors for secondary ImGui contexts.
    // Key: (ImGuiContext*, icon_name) → VkDescriptorSet
    struct ContextKey {
        void* context;
        std::string name;
        bool operator==(const ContextKey& o) const { return context == o.context && name == o.name; }
    };
    struct ContextKeyHash {
        size_t operator()(const ContextKey& k) const {
            return std::hash<void*>{}(k.context) ^ (std::hash<std::string>{}(k.name) << 1);
        }
    };
    std::unordered_map<ContextKey, VkDescriptorSet, ContextKeyHash> context_descriptors_;

    // Async thumbnail pipeline: background threads decode pixels, main thread uploads
    struct PendingThumbnail {
        std::string path;
        std::vector<uint8_t> pixels;
        int width;
        int height;
    };
    std::mutex pending_mutex_;
    std::vector<PendingThumbnail> pending_thumbnails_;

    // In-flight GPU upload — submitted but not yet finished
    struct InFlightUpload {
        VkFence fence{VK_NULL_HANDLE};
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        VkBuffer staging_buffer{VK_NULL_HANDLE};
        VkDeviceMemory staging_memory{VK_NULL_HANDLE};
        std::vector<std::string> paths;
        std::vector<IconEntry> entries;
    };
    InFlightUpload in_flight_;
};

} // namespace fjell
