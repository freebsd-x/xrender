#include "xrender/vulkan_context.h"
#include <iostream>

namespace fbx::xrender {

std::expected<std::unique_ptr<VulkanContext>, VulkanError> VulkanContext::create() noexcept {
    auto context = std::unique_ptr<VulkanContext>(new VulkanContext());
    auto res = context->init();
    if (!res.has_value()) {
        return std::unexpected(res.error());
    }
    return context;
}

VulkanContext::~VulkanContext() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
        std::cout << "[xrender] Vulkan Logical Device destroyed.\n";
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        std::cout << "[xrender] Vulkan Instance destroyed.\n";
    }
}

std::expected<void, VulkanError> VulkanContext::init() noexcept {
    if (auto res = createInstance(); !res.has_value()) return res;
    if (auto res = pickPhysicalDevice(); !res.has_value()) return res;
    if (auto res = createLogicalDevice(); !res.has_value()) return res;
    loadExtensionFunctions();
    if (auto res = createCommandPool(); !res.has_value()) return res;
    return {};
}

std::expected<void, VulkanError> VulkanContext::createInstance() noexcept {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "FreeBSD-X xrender";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "xrender Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        return std::unexpected(VulkanError::InstanceCreationError);
    }
    std::cout << "[xrender] Vulkan 1.3 Instance created successfully.\n";
    return {};
}

std::expected<void, VulkanError> VulkanContext::pickPhysicalDevice() noexcept {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return std::unexpected(VulkanError::NoSuitableGpu);

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    for (const auto& dev : devices) {
        uint32_t ext_count;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available_exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, available_exts.data());
        
        bool has_mem = false, has_sync = false;
        for (const auto& ext : available_exts) {
            if (std::string(ext.extensionName) == VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) has_mem = true;
            if (std::string(ext.extensionName) == VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) has_sync = true;
        }

        if (!has_mem || !has_sync) continue;

        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, queue_families.data());

        for (uint32_t i = 0; i < queue_families.size(); ++i) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical_device_ = dev;
                graphics_queue_family_index_ = i;
                
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(dev, &props);
                std::cout << "[xrender] Selected Physical Device: " << props.deviceName << "\n";
                return {};
            }
        }
    }
    return std::unexpected(VulkanError::NoSuitableGpu);
}

std::expected<void, VulkanError> VulkanContext::createLogicalDevice() noexcept {
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info{};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_index_;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures device_features{};

    const std::vector<const char*> required_device_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
    };

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_create_info;
    create_info.pEnabledFeatures = &device_features;
    create_info.enabledExtensionCount = static_cast<uint32_t>(required_device_extensions.size());
    create_info.ppEnabledExtensionNames = required_device_extensions.data();

    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        return std::unexpected(VulkanError::LogicalDeviceCreationError);
    }

    vkGetDeviceQueue(device_, graphics_queue_family_index_, 0, &graphics_queue_);
    std::cout << "[xrender] Logical Device and Graphics Queue created.\n";
    return {};
}

void VulkanContext::loadExtensionFunctions() noexcept {
    vk_get_memory_fd_khr_ = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    vk_get_semaphore_fd_khr_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));

    if (!vk_get_memory_fd_khr_ || !vk_get_semaphore_fd_khr_) {
        std::cerr << "[xrender] Warning: external FD extensions not fully found. Zero-copy IPC may fail.\n";
    } else {
        std::cout << "[xrender] External FD extension functions loaded.\n";
    }
}

std::expected<fbx::xserver::UniqueFd, VulkanError> VulkanContext::exportMemoryFd(VkDeviceMemory memory) const noexcept {
    if (!vk_get_memory_fd_khr_) return std::unexpected(VulkanError::MemoryAllocationError);

    VkMemoryGetFdInfoKHR get_fd_info{};
    get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get_fd_info.memory = memory;
    get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd = -1;
    if (vk_get_memory_fd_khr_(device_, &get_fd_info, &fd) != VK_SUCCESS) {
        return std::unexpected(VulkanError::MemoryAllocationError);
    }

    return fbx::xserver::UniqueFd(fd);
}

std::expected<fbx::xserver::UniqueFd, VulkanError> VulkanContext::exportSemaphoreFd(VkSemaphore semaphore) const noexcept {
    if (!vk_get_semaphore_fd_khr_) return std::unexpected(VulkanError::MemoryAllocationError);

    VkSemaphoreGetFdInfoKHR get_fd_info{};
    get_fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    get_fd_info.semaphore = semaphore;
    get_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    int fd = -1;
    if (vk_get_semaphore_fd_khr_(device_, &get_fd_info, &fd) != VK_SUCCESS) {
        return std::unexpected(VulkanError::MemoryAllocationError);
    }

    return fbx::xserver::UniqueFd(fd);
}

std::expected<void, VulkanError> VulkanContext::createCommandPool() noexcept {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = graphics_queue_family_index_;

    if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        return std::unexpected(VulkanError::CommandPoolCreationError);
    }
    std::cout << "[xrender] Command Pool created.\n";
    return {};
}

std::expected<uint32_t, VulkanError> VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const noexcept {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return std::unexpected(VulkanError::MemoryAllocationError);
}

} // namespace fbx::xrender
