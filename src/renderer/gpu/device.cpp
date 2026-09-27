#include "renderer/gpu/device.hpp"
#include "renderer/gpu/window.hpp"
#include "core/log.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstring>
#include <set>
#include <stdexcept>

namespace fjell {

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user_data*/) {
    // VUID-vkCmdTraceRaysKHR-None-08608: the layer only clears its "dynamic
    // state set since bind" flags on a *graphics* pipeline bind, so a viewport
    // or scissor set by any earlier raster pass on the same command buffer is
    // reported at the next trace. The spec only forbids dynamic state set after
    // the ray-tracing pipeline was bound, which no pass does, and viewport and
    // scissor have no effect on ray tracing. Filtered rather than reordering
    // every raster pass around a layer quirk.
    if (data->pMessageIdName &&
        std::strcmp(data->pMessageIdName, "VUID-vkCmdTraceRaysKHR-None-08608") == 0) {
        return VK_FALSE;
    }

    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        FJELL_GFX_TRACE("Validation: {}", data->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        FJELL_GFX_DEBUG("Validation: {}", data->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        FJELL_GFX_WARN("Validation: {}", data->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        FJELL_GFX_ERROR("Validation: {}", data->pMessage);
        break;
    default:
        FJELL_GFX_WARN("Validation (unknown severity): {}", data->pMessage);
        break;
    }
    return VK_FALSE;
}

static VkResult create_debug_utils_messenger(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* create_info,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* messenger) {
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    return func ? func(instance, create_info, allocator, messenger)
                : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void destroy_debug_utils_messenger(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* allocator) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func) {
        func(instance, messenger, allocator);
    }
}

Device::Device(Window& window) : window_{window} {
    FJELL_GFX_INFO("Initializing Vulkan device");
    create_instance();
    setup_debug_messenger();
    create_surface();
    pick_physical_device();
    create_logical_device();
    FJELL_GFX_INFO("Vulkan device ready");
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE)
        vkDestroyDevice(device_, nullptr);
    if constexpr (enable_validation_) {
        if (debug_messenger_ != VK_NULL_HANDLE) {
            destroy_debug_utils_messenger(instance_, debug_messenger_, nullptr);
        }
    }
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

void Device::create_instance() {
    if constexpr (enable_validation_) {
        if (!check_validation_layer_support()) {
            throw std::runtime_error("Validation layers requested but not available");
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Fjell";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Fjell";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    auto extensions = get_required_extensions();

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    if (enable_validation_) {
        create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers_.size());
        create_info.ppEnabledLayerNames = validation_layers_.data();
    }

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }
    FJELL_GFX_DEBUG("Vulkan instance created (API 1.3, {} extensions, validation {})",
                    extensions.size(), enable_validation_ ? "on" : "off");
}

void Device::setup_debug_messenger() {
    if (!enable_validation_) return;

    VkDebugUtilsMessengerCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = debug_callback;

    if (create_debug_utils_messenger(instance_, &create_info, nullptr, &debug_messenger_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to set up debug messenger");
    }
}

void Device::create_surface() {
    surface_ = window_.create_surface(instance_);
}

void Device::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);

    if (count == 0) {
        throw std::runtime_error("No GPUs with Vulkan support found");
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (const auto& device : devices) {
        if (is_device_suitable(device)) {
            physical_device_ = device;
            break;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("No suitable GPU found");
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    gpu_name_ = props.deviceName;
    FJELL_GFX_INFO("GPU: {}", gpu_name_);

    // VK_EXT_mesh_shader is required. Fjell renders exclusively through the
    // mesh-shader path; the vertex-shader fallback was retired.
    device_extensions_.assign(required_device_extensions_.begin(),
                              required_device_extensions_.end());

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available_exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, available_exts.data());

    bool mesh_ext_present = false;
    for (const auto& ext : available_exts) {
        if (std::strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
            mesh_ext_present = true;
            break;
        }
    }
    if (!mesh_ext_present) {
        throw std::runtime_error(
            "GPU does not support VK_EXT_mesh_shader (required). "
            "Fjell requires a GPU with mesh shader support "
            "(NVIDIA Turing+, AMD RDNA2+, or Intel Arc).");
    }

    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features{};
    mesh_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &mesh_features;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features2);

    if (!mesh_features.taskShader || !mesh_features.meshShader) {
        throw std::runtime_error(
            "GPU advertises VK_EXT_mesh_shader but does not enable taskShader/meshShader features.");
    }

    mesh_shader_supported_ = true;
    device_extensions_.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);

    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_props{};
    mesh_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &mesh_props;
    vkGetPhysicalDeviceProperties2(physical_device_, &props2);

    mesh_shader_max_workgroup_size_ = mesh_props.maxMeshWorkGroupSize[0];
    mesh_shader_max_output_vertices_ = mesh_props.maxMeshOutputVertices;
    mesh_shader_max_output_primitives_ = mesh_props.maxMeshOutputPrimitives;
    FJELL_GFX_INFO("Mesh shaders enabled (max workgroup: {}, max output: {} verts / {} prims)",
                   mesh_shader_max_workgroup_size_,
                   mesh_shader_max_output_vertices_,
                   mesh_shader_max_output_primitives_);

    // VK_EXT_device_fault: on VK_ERROR_DEVICE_LOST, lets us query the faulting
    // address/vendor info to pin which GPU op lost the device. Optional.
    for (const auto& ext : available_exts) {
        if (std::strcmp(ext.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0) {
            device_fault_supported_ = true;
            break;
        }
    }
    if (device_fault_supported_) {
        device_extensions_.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        FJELL_GFX_INFO("VK_EXT_device_fault enabled (device-loss diagnostics)");
    } else {
        FJELL_GFX_INFO("VK_EXT_device_fault not available");
    }

    // Probe for VK_KHR_ray_tracing_pipeline + VK_KHR_acceleration_structure
    bool has_rt_pipeline = false;
    bool has_accel_struct = false;
    bool has_deferred_ops = false;
    for (const auto& ext : available_exts) {
        if (std::strcmp(ext.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0)
            has_rt_pipeline = true;
        if (std::strcmp(ext.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0)
            has_accel_struct = true;
        if (std::strcmp(ext.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0)
            has_deferred_ops = true;
    }

    if (has_rt_pipeline && has_accel_struct && has_deferred_ops) {
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_features{};
        rt_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features{};
        as_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        as_features.pNext = &rt_features;

        VkPhysicalDeviceFeatures2 rt_query{};
        rt_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        rt_query.pNext = &as_features;
        vkGetPhysicalDeviceFeatures2(physical_device_, &rt_query);

        if (rt_features.rayTracingPipeline && as_features.accelerationStructure) {
            ray_tracing_supported_ = true;
            device_extensions_.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            device_extensions_.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            device_extensions_.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            FJELL_GFX_INFO("Ray tracing supported");
        }
    }

    if (!ray_tracing_supported_) {
        FJELL_GFX_INFO("Ray tracing not available, DDGI will use SDF fallback");
    }
}

void Device::create_logical_device() {
    auto indices = find_queue_families(physical_device_);

    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    std::set<uint32_t> unique_families = {
        indices.graphics.value(),
        indices.present.value()
    };
    if (indices.async_compute.has_value()) {
        unique_families.insert(indices.async_compute.value());
    }
    if (indices.transfer.has_value()) {
        unique_families.insert(indices.transfer.value());
    }

    float priority = 1.0f;
    for (uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_create_infos.push_back(queue_info);
    }

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_device_, &supported);

    VkPhysicalDeviceFeatures features{};
    features.sparseBinding = supported.sparseBinding;
    features.samplerAnisotropy = VK_TRUE;
    features.depthClamp = VK_TRUE;
    features.fillModeNonSolid = VK_TRUE;
    features.multiDrawIndirect = VK_TRUE;
    features.drawIndirectFirstInstance = VK_TRUE;
    features.independentBlend = VK_TRUE;

    // Vulkan 1.1 shader draw parameters (for gl_BaseInstance)
    VkPhysicalDeviceVulkan11Features features_11{};
    features_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features_11.shaderDrawParameters = VK_TRUE;

    // Vulkan 1.2 descriptor indexing (for bindless textures)
    VkPhysicalDeviceVulkan12Features features_12{};
    features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features_12.descriptorIndexing = VK_TRUE;
    features_12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features_12.runtimeDescriptorArray = VK_TRUE;
    features_12.descriptorBindingPartiallyBound = VK_TRUE;
    features_12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features_12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features_12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    // Needed by RtReflectionPass: it shares DdgiPass's UBO buffer and
    // owns its own storage-image output, both rewritten per-frame.
    features_12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    features_12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    // Needed for cross-queue sync between graphics and async compute
    // submissions (Phase 3). Harmless when async isn't used.
    features_12.timelineSemaphore = VK_TRUE;
    // Required by vkCmdDrawMeshTasksIndirectCountEXT (two-pass meshlet OC).
    features_12.drawIndirectCount = VK_TRUE;

    // Vulkan 1.3 dynamic rendering
    VkPhysicalDeviceVulkan13Features features_13{};
    features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features_13.pNext = nullptr;
    features_13.dynamicRendering = VK_TRUE;
    features_13.synchronization2 = VK_TRUE;
    features_13.maintenance4 = VK_TRUE;
    features_13.shaderDemoteToHelperInvocation = VK_TRUE;

    // Chain: create_info → features_11 → features_12 → features_13 [→ mesh_shader_features]
    features_13.pNext = nullptr;

    // Build pNext chain tail: mesh shader → RT → acceleration structure
    // Each enabled feature struct chains onto features_13.pNext
    void* chain_tail = nullptr;

    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features{};
    if (mesh_shader_supported_) {
        mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        mesh_shader_features.taskShader = VK_TRUE;
        mesh_shader_features.meshShader = VK_TRUE;
        mesh_shader_features.pNext = chain_tail;
        chain_tail = &mesh_shader_features;
    }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features{};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_features{};
    if (ray_tracing_supported_) {
        as_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        as_features.accelerationStructure = VK_TRUE;
        as_features.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;
        as_features.pNext = chain_tail;
        chain_tail = &as_features;

        rt_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rt_features.rayTracingPipeline = VK_TRUE;
        rt_features.pNext = chain_tail;
        chain_tail = &rt_features;

        // bufferDeviceAddress is required for acceleration structures
        features_12.bufferDeviceAddress = VK_TRUE;
    }

    VkPhysicalDeviceFaultFeaturesEXT fault_features{};
    if (device_fault_supported_) {
        fault_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        fault_features.deviceFault = VK_TRUE;
        fault_features.pNext = chain_tail;
        chain_tail = &fault_features;
    }

    features_13.pNext = chain_tail;

    features_12.pNext = &features_13;
    features_11.pNext = &features_12;

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &features_11;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();
    create_info.pEnabledFeatures = &features;
    create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions_.size());
    create_info.ppEnabledExtensionNames = device_extensions_.data();

    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    vkGetDeviceQueue(device_, indices.graphics.value(), 0, &graphics_queue_);
    vkGetDeviceQueue(device_, indices.present.value(), 0, &present_queue_);
    if (indices.async_compute.has_value()) {
        vkGetDeviceQueue(device_, indices.async_compute.value(), 0, &async_compute_queue_);
        concurrent_families_[0] = indices.graphics.value();
        concurrent_families_[1] = indices.async_compute.value();
        concurrent_family_count_ = 2;
        FJELL_GFX_INFO("Async compute supported (queue family {})",
                       indices.async_compute.value());
    } else {
        FJELL_GFX_INFO("Async compute not supported (no dedicated compute queue family)");
    }

    // Upload sharing families: graphics always; dedicated transfer and
    // async compute when present. Two or more entries means upload
    // destinations need VK_SHARING_MODE_CONCURRENT.
    upload_families_[upload_family_count_++] = indices.graphics.value();
    if (indices.transfer.has_value()) {
        vkGetDeviceQueue(device_, indices.transfer.value(), 0, &transfer_queue_);
        upload_families_[upload_family_count_++] = indices.transfer.value();
        FJELL_GFX_INFO("Dedicated transfer queue supported (queue family {})",
                       indices.transfer.value());
    } else {
        FJELL_GFX_INFO("No dedicated transfer queue family — uploads use the graphics queue");
    }
    if (indices.async_compute.has_value()) {
        upload_families_[upload_family_count_++] = indices.async_compute.value();
    }

    // Sparse bind queue: prefer the dedicated transfer queue so binds never
    // share a queue with rendering; fall back to graphics. Sparse bind
    // operations only order against semaphores (not command buffers), so
    // either way a bind completes in microseconds.
    if (supported.sparseBinding == VK_TRUE) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());
        auto family_has_sparse = [&](uint32_t family) {
            return (families[family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
        };
        if (indices.transfer.has_value() && family_has_sparse(indices.transfer.value())) {
            sparse_bind_queue_ = transfer_queue_;
        } else if (family_has_sparse(indices.graphics.value())) {
            sparse_bind_queue_ = graphics_queue_;
        }
    }
    if (sparse_bind_queue_ != VK_NULL_HANDLE) {
        FJELL_GFX_INFO("Sparse binding supported (binds on {} queue)",
                       sparse_bind_queue_ == transfer_queue_ ? "transfer" : "graphics");
    } else {
        FJELL_GFX_INFO("Sparse binding not supported — growable buffers fall back to copy-grow");
    }

    // Load mesh shader extension function pointers
    if (mesh_shader_supported_) {
        pfn_draw_mesh_tasks_ = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdDrawMeshTasksEXT"));
        pfn_draw_mesh_tasks_indirect_ = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdDrawMeshTasksIndirectEXT"));
        pfn_draw_mesh_tasks_indirect_count_ = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectCountEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdDrawMeshTasksIndirectCountEXT"));
    }

    // Load ray tracing extension function pointers
    if (ray_tracing_supported_) {
        pfn_create_rt_pipelines_ = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
            vkGetDeviceProcAddr(device_, "vkCreateRayTracingPipelinesKHR"));
        pfn_cmd_trace_rays_ = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdTraceRaysKHR"));
        pfn_get_rt_shader_group_handles_ = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetRayTracingShaderGroupHandlesKHR"));
        pfn_create_accel_struct_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
        pfn_destroy_accel_struct_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
        pfn_get_accel_struct_build_sizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
        pfn_cmd_build_accel_structs_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
        pfn_get_accel_struct_device_address_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureDeviceAddressKHR"));
        pfn_get_buffer_device_address_ = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            vkGetDeviceProcAddr(device_, "vkGetBufferDeviceAddress"));
    }
}

const uint32_t* Device::concurrent_queue_families(uint32_t& out_count) const {
    if (concurrent_family_count_ == 0) {
        out_count = 0;
        return nullptr;
    }
    out_count = concurrent_family_count_;
    return concurrent_families_.data();
}

QueueFamilyIndices Device::find_queue_families() const {
    return find_queue_families(physical_device_);
}

QueueFamilyIndices Device::find_queue_families(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics = i;
        }

        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present_support);
        if (present_support) {
            indices.present = i;
        }
    }

    // Async compute: prefer a dedicated family (COMPUTE bit, no GRAPHICS
    // bit) — those run in parallel with graphics work. If every compute-
    // capable family also has GRAPHICS, skip async support entirely
    // rather than pretending (same family = same hardware scheduler,
    // submissions serialize).
    for (uint32_t i = 0; i < count; ++i) {
        bool has_compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        bool has_graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (has_compute && !has_graphics) {
            indices.async_compute = i;
            break;
        }
    }

    // Dedicated transfer: the DMA engine family (TRANSFER bit, no GRAPHICS
    // or COMPUTE). Copies submitted there overlap with rendering instead of
    // contending for the graphics queue. Same reasoning as async compute:
    // a shared family wouldn't actually run in parallel, so skip it.
    for (uint32_t i = 0; i < count; ++i) {
        bool has_transfer = (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        bool has_graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        bool has_compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (has_transfer && !has_graphics && !has_compute) {
            indices.transfer = i;
            break;
        }
    }

    return indices;
}

SwapchainSupport Device::query_swapchain_support() const {
    return query_swapchain_support(physical_device_, surface_);
}

SwapchainSupport Device::query_swapchain_support(VkSurfaceKHR surface) const {
    return query_swapchain_support(physical_device_, surface);
}

SwapchainSupport Device::query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface) const {
    SwapchainSupport support;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &support.capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    if (format_count > 0) {
        support.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, support.formats.data());
    }

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, nullptr);
    if (mode_count > 0) {
        support.present_modes.resize(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, support.present_modes.data());
    }

    return support;
}

bool Device::is_device_suitable(VkPhysicalDevice device) const {
    auto indices = find_queue_families(device);
    bool extensions_ok = check_device_extension_support(device);

    bool swapchain_ok = false;
    if (extensions_ok) {
        auto support = query_swapchain_support(device, surface_);
        swapchain_ok = !support.formats.empty() && !support.present_modes.empty();
    }

    return indices.is_complete() && extensions_ok && swapchain_ok;
}

bool Device::check_device_extension_support(VkPhysicalDevice device) const {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    std::set<std::string> required(device_extensions_.begin(), device_extensions_.end());
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

bool Device::check_validation_layer_support() const {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);

    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());

    for (const char* layer : validation_layers_) {
        bool found = false;
        for (const auto& props : available) {
            if (std::strcmp(layer, props.layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

std::vector<const char*> Device::get_required_extensions() const {
    uint32_t glfw_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_count);

    std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_count);

    if (enable_validation_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

VkFormat Device::find_depth_format() const {
    return find_supported_format(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

VkFormat Device::find_supported_format(
    const std::vector<VkFormat>& candidates, VkImageTiling tiling,
    VkFormatFeatureFlags features) const {
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }
    throw std::runtime_error("Failed to find supported format");
}

void Device::dump_device_fault(const char* context) const {
    if (!device_fault_supported_) {
        FJELL_GFX_CRITICAL("DEVICE_LOST during '{}' — VK_EXT_device_fault unavailable, "
                           "cannot query faulting op", context);
        return;
    }

    auto pfn = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(device_, "vkGetDeviceFaultInfoEXT"));
    if (!pfn) {
        FJELL_GFX_CRITICAL("DEVICE_LOST during '{}' — vkGetDeviceFaultInfoEXT not loaded", context);
        return;
    }

    VkDeviceFaultCountsEXT counts{};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    if (pfn(device_, &counts, nullptr) != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("DEVICE_LOST during '{}' — vkGetDeviceFaultInfoEXT(counts) failed", context);
        return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    std::vector<std::byte> vendor_bin(counts.vendorBinarySize);

    VkDeviceFaultInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = addrs.empty() ? nullptr : addrs.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    info.pVendorBinaryData = vendor_bin.empty() ? nullptr : vendor_bin.data();
    if (pfn(device_, &counts, &info) != VK_SUCCESS) {
        FJELL_GFX_CRITICAL("DEVICE_LOST during '{}' — vkGetDeviceFaultInfoEXT(data) failed", context);
        return;
    }

    FJELL_GFX_CRITICAL("=== DEVICE FAULT during '{}' ===", context);
    FJELL_GFX_CRITICAL("  description: {}", info.description);
    FJELL_GFX_CRITICAL("  address infos: {}, vendor infos: {}, vendor binary: {} bytes",
                       counts.addressInfoCount, counts.vendorInfoCount, counts.vendorBinarySize);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        const auto& a = addrs[i];
        const char* kind = "?";
        switch (a.addressType) {
            case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: kind = "READ_INVALID"; break;
            case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: kind = "WRITE_INVALID"; break;
            case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: kind = "EXECUTE_INVALID"; break;
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: kind = "IP_UNKNOWN"; break;
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: kind = "IP_INVALID"; break;
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: kind = "IP_FAULT"; break;
            default: break;
        }
        FJELL_GFX_CRITICAL("  [addr {}] type={} reported=0x{:x} precision=0x{:x}",
                           i, kind, a.reportedAddress, a.addressPrecision);
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        const auto& v = vendors[i];
        FJELL_GFX_CRITICAL("  [vendor {}] '{}' code=0x{:x} data=0x{:x}",
                           i, v.description, v.vendorFaultCode, v.vendorFaultData);
    }
}

VkSampleCountFlagBits Device::max_msaa_samples() const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);

    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                props.limits.framebufferDepthSampleCounts;

    if (counts & VK_SAMPLE_COUNT_64_BIT) return VK_SAMPLE_COUNT_64_BIT;
    if (counts & VK_SAMPLE_COUNT_32_BIT) return VK_SAMPLE_COUNT_32_BIT;
    if (counts & VK_SAMPLE_COUNT_16_BIT) return VK_SAMPLE_COUNT_16_BIT;
    if (counts & VK_SAMPLE_COUNT_8_BIT)  return VK_SAMPLE_COUNT_8_BIT;
    if (counts & VK_SAMPLE_COUNT_4_BIT)  return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT)  return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

} // namespace fjell
