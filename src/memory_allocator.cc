#include "xrender/memory_allocator.h"

#include <sys/mman.h>
#include <cerrno>
#include <iostream>
#include <utility>

namespace fbx::xrender {

MemoryBlock::~MemoryBlock() noexcept {
    if (data_ && size_ > 0) {
        munmap(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }
}

MemoryBlock::MemoryBlock(MemoryBlock&& other) noexcept 
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MemoryBlock& MemoryBlock::operator=(MemoryBlock&& other) noexcept {
    if (this != &other) {
        if (data_) {
            munmap(data_, size_);
        }
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

std::expected<MemoryBlock, std::error_code> MemoryAllocator::allocateSuper(size_t size) noexcept {
    if (size == 0) return MemoryBlock{};

    constexpr size_t SUPERPAGE_SIZE = 2 * 1024 * 1024; // 2MB
    size_t aligned_size = ((size + SUPERPAGE_SIZE - 1) / SUPERPAGE_SIZE) * SUPERPAGE_SIZE;

    // FreeBSD specific flags for superpage alignment
    int flags = MAP_PRIVATE | MAP_ANON | MAP_ALIGNED_SUPER;
    
    void* data = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    
    if (data == MAP_FAILED) {
        std::cerr << "[xrender] Fallback to standard mmap for size " << aligned_size << "\n";
        flags = MAP_PRIVATE | MAP_ANON;
        data = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, flags, -1, 0);
        
        if (data == MAP_FAILED) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
    } else {
        std::cout << "[xrender] Successfully allocated superpage memory block of size " << aligned_size << "\n";
    }

    return MemoryBlock(data, aligned_size);
}

} // namespace fbx::xrender
