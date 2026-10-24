#pragma once

#include "xrender/image_decoder.h"
#include <expected>
#include <system_error>
#include <string_view>

namespace fbx::xrender {

// Minimal-but-real SVG rasterizer. Supports the subset the platform's icon and
// branding assets actually use: <rect> (with rx rounded corners), <path>
// (M/L/H/V/C/S/Q/T/A/Z, absolute and relative), solid and 2-stop linear
// gradient fills, fill-opacity, and stroked paths with round caps. Geometry is
// flattened to polylines and filled with a supersampled non-zero-winding
// scanline; strokes are rasterized by distance-to-segment. Output is straight
// (non-premultiplied) RGBA at the requested target size.
class SvgEngine {
public:
    static std::expected<ImageData, std::error_code>
    decode(std::string_view svg_data, uint32_t target_w, uint32_t target_h) noexcept;
};

} // namespace fbx::xrender
