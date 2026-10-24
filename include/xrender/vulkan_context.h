#pragma once

#include "xserver/unique_fd.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <expected>
#include <string>

namespace fbx::xrender {

// Define an error enum for Vulkan failures to integrate with std::expected
enum class VulkanError {
    InstanceCreationError,
    NoSuitableGpu,
    LogicalDeviceCreationError,
    CommandPoolCreationError,
    MemoryAllocationError,
    ShaderCompilationError,
    PipelineCreationError,
    CommandBufferRecordingError
};

class VulkanContext {
public:
    static std::expected<std::unique_ptr<VulkanContext>, VulkanError> create() noexcept;

    ~VulkanContext() noexcept;

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] VkInstance getInstance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const noexcept { return physical_device_; }
    [[nodiscard]] VkDevice getDevice() const noexcept { return device_; }
    [[nodiscard]] VkQueue getGraphicsQueue() const noexcept { return graphics_queue_; }
    [[nodiscard]] VkCommandPool getCommandPool() const noexcept { return command_pool_; }

    [[nodiscard]] std::expected<uint32_t, VulkanError> findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const noexcept;

    // IPC zero-copy memory sharing
    [[nodiscard]] std::expected<fbx::xserver::UniqueFd, VulkanError> exportMemoryFd(VkDeviceMemory memory) const noexcept;
    [[nodiscard]] std::expected<fbx::xserver::UniqueFd, VulkanError> exportSemaphoreFd(VkSemaphore semaphore) const noexcept;

private:
    VulkanContext() noexcept = default;

    std::expected<void, VulkanError> init() noexcept;
    std::expected<void, VulkanError> createInstance() noexcept;
    std::expected<void, VulkanError> pickPhysicalDevice() noexcept;
    std::expected<void, VulkanError> createLogicalDevice() noexcept;
    std::expected<void, VulkanError> createCommandPool() noexcept;
    void loadExtensionFunctions() noexcept;

    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    
    uint32_t graphics_queue_family_index_{0};

    // Extension functions
    PFN_vkGetMemoryFdKHR vk_get_memory_fd_khr_{nullptr};
    PFN_vkGetSemaphoreFdKHR vk_get_semaphore_fd_khr_{nullptr};
};

} // namespace fbx::xrender
