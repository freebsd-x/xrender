#pragma once

#include <cstddef>
#include <memory>
#include <expected>
#include <system_error>

namespace fbx::xrender {

class MemoryBlock {
public:
    constexpr MemoryBlock() noexcept = default;
    MemoryBlock(void* data, size_t size) noexcept : data_(data), size_(size) {}
    ~MemoryBlock() noexcept;

    // Prevent copying
    MemoryBlock(const MemoryBlock&) = delete;
    MemoryBlock& operator=(const MemoryBlock&) = delete;

    // Allow moving
    MemoryBlock(MemoryBlock&& other) noexcept;
    MemoryBlock& operator=(MemoryBlock&& other) noexcept;

    [[nodiscard]] void* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }

private:
    void* data_{nullptr};
    size_t size_{0};
};

class MemoryAllocator {
public:
    // Allocates memory optimized for high-bandwidth transfers
    // utilizing FreeBSD's superpages (MAP_ALIGNED_SUPER) where possible.
    [[nodiscard]] static std::expected<MemoryBlock, std::error_code> allocateSuper(size_t size) noexcept;
};

} // namespace fbx::xrender
