#pragma once

#include <string>
#include <vector>
#include <expected>
#include <system_error>
#include <cstdint>

namespace fbx::xrender {

struct ImageData {
    std::vector<uint8_t> pixels_; // RGBA 8-bit per channel
    uint32_t width_;
    uint32_t height_;
};

class ImageDecoder {
public:
    static std::expected<ImageData, std::error_code> decode(const std::string& path, uint32_t target_w = 64, uint32_t target_h = 64) noexcept;
    static std::expected<ImageData, std::error_code> decodePng(const std::string& path) noexcept;
    static std::expected<ImageData, std::error_code> decodeSvg(const std::string& path, uint32_t target_w, uint32_t target_h) noexcept;
};

} // namespace fbx::xrender
