#include "gpu/swapchain.hpp"

#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/frame_impl.hpp"
#include "gpu/vulkan/native.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/window.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <string>
#include <vector>

// The Vulkan swapchain: a VkSwapchainKHR on the window's surface, its images
// adopted as textures, and the semaphores that tie each image to the frame
// drawing it and to present.

namespace fjell::gpu {

namespace {

std::string failed(const char* what, VkResult result) {
    return std::string(what) + " (VkResult=" + std::to_string(static_cast<int>(result)) + ")";
}

// The format the images are made in: 8-bit sRGB BGRA where the window
// offers it, as nearly every one does, else the first format the interface
// names.
std::optional<VkSurfaceFormatKHR> choose_format(const std::vector<VkSurfaceFormatKHR>& offered) {
    for (const auto& f : offered) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (const auto& f : offered) {
        if (vulkan::from_vk(f.format) != Format::undefined) return f;
    }
    return std::nullopt;
}

// Mailbox where the window offers it, so a frame never waits for the
// display; else first in, first out, which every window offers.
VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& offered) {
    const bool mailbox = std::ranges::find(offered, VK_PRESENT_MODE_MAILBOX_KHR) != offered.end();
    return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

// The surface's own size where it has one, else the window's drawable size
// within what the surface allows.
VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D drawable) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    return {std::clamp(drawable.width, capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width),
            std::clamp(drawable.height, capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height)};
}

} // namespace

struct Swapchain::Impl {
    Impl(Device& gpu_device, Window& shown, VkSurfaceKHR on, bool own_surface)
        : device(gpu_device), vk(gpu_device.impl().core.device()), window(shown), surface(on),
          owns_surface(own_surface) {}

    ~Impl() {
        tear_down();
        if (owns_surface) vkDestroySurfaceKHR(vk.instance(), surface, nullptr);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// Makes the swapchain at the window's drawable size, waiting while the
    /// window is minimised, with a texture and a semaphore per image.
    Result<> build();

    /// Destroys what `build` made. The GPU must be done with it.
    void tear_down();

    /// Waits for the GPU and builds again at the window's size now.
    void rebuild();

    Device& device;
    fjell::Device& vk;
    Window& window;
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    /// A window other than the one the device was made for brings its own
    /// surface, which the swapchain destroys.
    bool owns_surface{false};

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    Format format{Format::undefined};
    VkExtent2D extent{};
    /// Per image: the whole view, made here, and the adopted texture.
    std::vector<VkImageView> views;
    std::vector<Owned<Texture>> textures;
    /// Per image: what the frame drawing it signals and present waits on.
    /// One per image, not per frame: only acquiring the image again shows
    /// present is done with it.
    std::vector<VkSemaphore> rendered;
    /// What acquires signal, taken in turn: one more than the images, so
    /// the next one is never still waited on by a frame the GPU has not
    /// finished.
    std::vector<VkSemaphore> acquired;
    size_t next_acquired{0};
};

Result<> Swapchain::Impl::build() {
    VkExtent2D drawable = window.framebuffer_size();
    while (drawable.width == 0 || drawable.height == 0) {
        window.wait_events();
        drawable = window.framebuffer_size();
    }

    const VkDevice dev = device.impl().device;
    const SwapchainSupport support = vk.query_swapchain_support(surface);
    const auto surface_format = choose_format(support.formats);
    if (!surface_format) return make_error("The window offers no image format the GPU interface names");

    uint32_t count = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0) {
        count = std::min(count, support.capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = count;
    info.imageFormat = surface_format->format;
    info.imageColorSpace = surface_format->colorSpace;
    info.imageExtent = choose_extent(support.capabilities, drawable);
    info.imageArrayLayers = 1;
    // Sampled where the window allows it, so a frame shown can be read back.
    const bool sampled =
        (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (sampled) info.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto families = vk.find_queue_families();
    const std::array<uint32_t, 2> both{families.graphics.value(), families.present.value()};
    if (both[0] != both[1]) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = both.data();
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = support.capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = choose_present_mode(support.present_modes);
    info.clipped = VK_TRUE;
    if (VkResult made = vkCreateSwapchainKHR(dev, &info, nullptr, &swapchain); made != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return make_error(failed("The swapchain could not be made", made));
    }
    format = vulkan::from_vk(surface_format->format);
    extent = info.imageExtent;

    vkGetSwapchainImagesKHR(dev, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(dev, swapchain, &count, images.data());

    TextureInfo texture_info;
    texture_info.format = format;
    texture_info.width = extent.width;
    texture_info.height = extent.height;
    texture_info.use = TextureUse::color_target;
    if (sampled) texture_info.use |= TextureUse::sampled;
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface_format->format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{VK_NULL_HANDLE};
        if (VkResult made = vkCreateImageView(dev, &view_info, nullptr, &view); made != VK_SUCCESS) {
            return make_error(failed("A swapchain image's view could not be made", made));
        }
        views.push_back(view);
        textures.push_back(vulkan::adopt(device, images[i], view, texture_info));
        vulkan::name_object(dev, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(images[i]),
                            "swapchain image " + std::to_string(i));

        VkSemaphore semaphore{VK_NULL_HANDLE};
        if (VkResult made = vkCreateSemaphore(dev, &semaphore_info, nullptr, &semaphore);
            made != VK_SUCCESS) {
            return make_error(failed("A swapchain semaphore could not be made", made));
        }
        rendered.push_back(semaphore);
    }
    for (uint32_t i = 0; i <= count; ++i) {
        VkSemaphore semaphore{VK_NULL_HANDLE};
        if (VkResult made = vkCreateSemaphore(dev, &semaphore_info, nullptr, &semaphore);
            made != VK_SUCCESS) {
            return make_error(failed("A swapchain semaphore could not be made", made));
        }
        acquired.push_back(semaphore);
    }
    next_acquired = 0;
    return {};
}

void Swapchain::Impl::tear_down() {
    const VkDevice dev = device.impl().device;
    textures.clear();
    for (VkImageView view : views) vkDestroyImageView(dev, view, nullptr);
    views.clear();
    for (VkSemaphore semaphore : rendered) vkDestroySemaphore(dev, semaphore, nullptr);
    rendered.clear();
    for (VkSemaphore semaphore : acquired) vkDestroySemaphore(dev, semaphore, nullptr);
    acquired.clear();
    if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(dev, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void Swapchain::Impl::rebuild() {
    // Present gives no word of when it is done with an image or a semaphore
    // but a later acquire; with the swapchain going, only an idle GPU does.
    vkDeviceWaitIdle(device.impl().device);
    tear_down();
    if (auto built = build(); !built) device.impl().report_once(built.error());
}

// ── Swapchain ───────────────────────────────────────────────────────────

Result<std::unique_ptr<Swapchain>> Swapchain::create(Device& device, Window& window) {
    Device::Impl& self = device.impl();
    fjell::Device& vk = self.core.device();
    std::unique_ptr<Impl> impl;
    if (&window == &self.core.window()) {
        impl = std::make_unique<Impl>(device, window, vk.surface(), false);
    } else {
        VkSurfaceKHR surface{VK_NULL_HANDLE};
        try {
            surface = window.create_surface(vk.instance());
        } catch (const std::exception& e) {
            return make_error(e.what());
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical_device(),
                                             vk.find_queue_families().present.value(), surface,
                                             &supported);
        if (supported == VK_FALSE) {
            vkDestroySurfaceKHR(vk.instance(), surface, nullptr);
            return make_error("The GPU cannot present to the window");
        }
        impl = std::make_unique<Impl>(device, window, surface, true);
    }
    if (auto built = impl->build(); !built) return std::unexpected(built.error());
    return std::make_unique<Swapchain>(device, std::move(impl));
}

Swapchain::Swapchain(Device& device, std::unique_ptr<Impl> impl)
    : device_(&device), impl_(std::move(impl)) {}

Swapchain::~Swapchain() {
    vkDeviceWaitIdle(device_->impl().device);
}

std::optional<SwapchainImage> Swapchain::acquire(Frame& frame) {
    Device::Impl& device = device_->impl();
    Frame::Impl& f = frame.impl();
    if (!f.open) {
        device.report_once("A swapchain image is acquired for a frame that is not recording");
        return std::nullopt;
    }
    if (f.acquired != VK_NULL_HANDLE) {
        device.report_once("A frame acquires a second swapchain image");
        return std::nullopt;
    }
    if (impl_->swapchain == VK_NULL_HANDLE) return std::nullopt;

    const VkSemaphore semaphore = impl_->acquired[impl_->next_acquired];
    uint32_t index = 0;
    const VkResult result =
        vkAcquireNextImageKHR(device.device, impl_->swapchain, std::numeric_limits<uint64_t>::max(),
                              semaphore, VK_NULL_HANDLE, &index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        impl_->next_acquired = (impl_->next_acquired + 1) % impl_->acquired.size();
        f.acquired = semaphore;
        f.rendered = impl_->rendered[index];
        return SwapchainImage{.texture = impl_->textures[index].get(), .index = index};
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        impl_->rebuild();
    } else {
        device.report_once(failed("The swapchain could not acquire an image", result));
    }
    return std::nullopt;
}

Result<> Swapchain::present(Frame& frame, const SwapchainImage& image) {
    Device::Impl& device = device_->impl();
    if (frame.impl().rendered != impl_->rendered[image.index]) {
        device.report_once("A frame presents a swapchain image it did not acquire");
    }
    if (auto sent = device_->end_frame(frame); !sent) return sent;

    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &impl_->rendered[image.index];
    info.swapchainCount = 1;
    info.pSwapchains = &impl_->swapchain;
    info.pImageIndices = &image.index;
    const VkResult result = vkQueuePresentKHR(impl_->vk.present_queue(), &info);

    // A compositor may resize the window without the swapchain going out of
    // date, which leaves the images at their old size.
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        impl_->window.was_resized()) {
        impl_->window.reset_resized();
        impl_->rebuild();
        return {};
    }
    if (result != VK_SUCCESS) return make_error(failed("The window refused a frame", result));
    return {};
}

Format Swapchain::format() const {
    return impl_->format;
}

uint32_t Swapchain::width() const {
    return impl_->extent.width;
}

uint32_t Swapchain::height() const {
    return impl_->extent.height;
}

} // namespace fjell::gpu
