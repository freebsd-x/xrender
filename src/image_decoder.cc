#include "xrender/image_decoder.h"
#include "xrender/svg_engine.h"
#include <png.h>
#include <iostream>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <unordered_map>

namespace fbx::xrender {

namespace {
// Memoize decoded images so the same asset at the same size isn't re-decoded —
// and for SVGs re-rasterized at 4x supersampling — every time an ImageElement
// is reconstructed during UI reconciliation (the logo/icon re-decode spam).
// Keyed by path+size, validated by file mtime so an edited asset re-decodes.
// The UI runs single-threaded, so no lock is needed.
struct CachedImage {
    std::filesystem::file_time_type mtime;
    ImageData data;
};
std::unordered_map<std::string, CachedImage> g_decode_cache;
} // namespace

std::expected<ImageData, std::error_code> ImageDecoder::decode(const std::string& path, uint32_t target_w, uint32_t target_h) noexcept {
    const std::string key = path + '|' + std::to_string(target_w) + 'x' + std::to_string(target_h);

    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        auto it = g_decode_cache.find(key);
        if (it != g_decode_cache.end() && it->second.mtime == mtime) {
            return it->second.data;   // copy of cached pixels — no re-decode
        }
    }

    std::filesystem::path p(path);
    auto res = (p.extension() == ".svg") ? decodeSvg(path, target_w, target_h)
                                         : decodePng(path);
    if (res && !ec) {
        g_decode_cache[key] = CachedImage{mtime, res.value()};
    }
    return res;
}

std::expected<ImageData, std::error_code> ImageDecoder::decodePng(const std::string& path) noexcept {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        std::fclose(fp);
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int width = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);

    std::vector<png_bytep> row_pointers(height);
    std::vector<uint8_t> pixels(width * height * 4);

    for (int y = 0; y < height; y++) {
        row_pointers[y] = pixels.data() + y * width * 4;
    }

    png_read_image(png, row_pointers.data());

    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);

    std::cout << "[xrender] Decoded PNG: " << path << " (" << width << "x" << height << ")\n";

    return ImageData{
        .pixels_ = std::move(pixels),
        .width_ = static_cast<uint32_t>(width),
        .height_ = static_cast<uint32_t>(height)
    };
}

std::expected<ImageData, std::error_code> ImageDecoder::decodeSvg(const std::string& path, uint32_t target_w, uint32_t target_h) noexcept {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::cout << "[xrender] Loading custom SvgEngine for: " << path << "\n";
    
    return SvgEngine::decode(content, target_w, target_h);
}

} // namespace fbx::xrender
