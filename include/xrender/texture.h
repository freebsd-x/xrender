#pragma once

#include <vulkan/vulkan.h>
#include <expected>
#include <system_error>
#include <span>
#include "xrender/vulkan_context.h"

namespace fbx::xrender {

class Texture {
public:
    static std::expected<std::unique_ptr<Texture>, VulkanError> create(
        const VulkanContext& context,
        uint32_t width,
        uint32_t height,
        VkFormat format
    ) noexcept;

    ~Texture() noexcept;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    [[nodiscard]] VkImage getImage() const noexcept { return image_; }
    [[nodiscard]] VkImageView getImageView() const noexcept { return view_; }
    [[nodiscard]] uint32_t getWidth() const noexcept { return width_; }
    [[nodiscard]] uint32_t getHeight() const noexcept { return height_; }

    std::expected<void, VulkanError> upload(std::span<const uint8_t> data) noexcept;

private:
    Texture(const VulkanContext& context, uint32_t width, uint32_t height, VkFormat format) noexcept;

    const VulkanContext& context_;
    uint32_t width_;
    uint32_t height_;
    VkFormat format_;

    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
};

} // namespace fbx::xrender
