// Regression tests for the SVG rasterizer. These lock in the behaviour that
// replaced the old placeholder stub (which filled every pixel with an opaque
// dark box regardless of geometry — the cause of "icons render as blank/dark
// boxes"). The assertions below fail if anyone re-stubs the engine.
#include "xrender/svg_engine.h"
#include <gtest/gtest.h>

using fbx::xrender::SvgEngine;

namespace {

// Alpha of pixel (x,y) in a decoded RGBA8 image.
uint8_t alphaAt(const fbx::xrender::ImageData& img, uint32_t x, uint32_t y) {
    return img.pixels_[(y * img.width_ + x) * 4 + 3];
}
struct Px { uint8_t r, g, b, a; };
Px pixel(const fbx::xrender::ImageData& img, uint32_t x, uint32_t y) {
    size_t i = (y * img.width_ + x) * 4;
    return {img.pixels_[i], img.pixels_[i+1], img.pixels_[i+2], img.pixels_[i+3]};
}

// The gradient-stroked "X" brand logo.
constexpr const char* kLogoSvg =
    R"SVG(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
    <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#ffffff;stop-opacity:1"/>
      <stop offset="100%" style="stop-color:#0078d7;stop-opacity:1"/>
    </linearGradient></defs>
    <path d="M20 20 L80 80 M80 20 L20 80" stroke="url(#g)" stroke-width="12"
          stroke-linecap="round"/></svg>)SVG";

// A rounded-rect gradient tile with a white plus glyph (an app-icon shape).
constexpr const char* kIconSvg =
    R"SVG(<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
    <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#00aa55;stop-opacity:1"/>
      <stop offset="100%" style="stop-color:#000000;stop-opacity:1"/>
    </linearGradient></defs>
    <rect width="100" height="100" rx="22" fill="url(#g)"/>
    <path d="M45 30 L55 30 L55 45 L70 45 L70 55 L55 55 L55 70 L45 70 L45 55 L30 55 L30 45 L45 45 Z"
          fill="white"/></svg>)SVG";

// The stub filled the entire canvas opaque; a real rasterizer of an "X" stroke
// leaves the corners transparent. This is the core anti-regression guard.
TEST(SvgEngine, StrokedLogoHasTransparentCornersAndOpaqueCenter) {
    auto r = SvgEngine::decode(kLogoSvg, 32, 32);
    ASSERT_TRUE(r.has_value());
    const auto& img = r.value();
    ASSERT_EQ(img.width_, 32u);
    ASSERT_EQ(img.height_, 32u);

    // The four corners are outside the X stroke → transparent.
    EXPECT_EQ(alphaAt(img, 0, 0), 0);
    EXPECT_EQ(alphaAt(img, 31, 0), 0);
    EXPECT_EQ(alphaAt(img, 0, 31), 0);
    EXPECT_EQ(alphaAt(img, 31, 31), 0);

    // The strokes cross at the centre → opaque.
    EXPECT_GT(alphaAt(img, 16, 16), 200);

    // Not a uniform fill: there must be a healthy mix of transparent and opaque.
    int opaque = 0, clear = 0;
    for (uint32_t y = 0; y < img.height_; ++y)
        for (uint32_t x = 0; x < img.width_; ++x)
            (alphaAt(img, x, y) > 200 ? opaque : clear) += 1;
    EXPECT_GT(opaque, 40);
    EXPECT_GT(clear, 200); // most of the frame is background for a thin X
}

TEST(SvgEngine, RoundedRectHasTransparentCornersAndSolidBody) {
    auto r = SvgEngine::decode(kIconSvg, 32, 32);
    ASSERT_TRUE(r.has_value());
    const auto& img = r.value();

    // rx=22 rounds the corners away → corner pixels transparent.
    EXPECT_EQ(alphaAt(img, 0, 0), 0);
    EXPECT_EQ(alphaAt(img, 31, 31), 0);
    // Body is filled.
    EXPECT_GT(alphaAt(img, 16, 16), 200);
    EXPECT_GT(alphaAt(img, 8, 8), 200);
}

TEST(SvgEngine, LinearGradientVariesAcrossTheTile) {
    auto r = SvgEngine::decode(kIconSvg, 32, 32);
    ASSERT_TRUE(r.has_value());
    const auto& img = r.value();
    // Top-left stop is bright green (#00aa55), bottom-right fades to black.
    Px tl = pixel(img, 6, 6);
    Px br = pixel(img, 26, 26);
    // The green channel should be clearly brighter near the first stop than the
    // last — proving the gradient is actually sampled, not a flat fill.
    EXPECT_GT(tl.g, br.g + 30);
}

TEST(SvgEngine, DecodesAtMultipleTargetSizes) {
    for (uint32_t s : {16u, 24u, 64u, 128u}) {
        auto r = SvgEngine::decode(kIconSvg, s, s);
        ASSERT_TRUE(r.has_value()) << "size " << s;
        EXPECT_EQ(r->width_, s);
        EXPECT_EQ(r->pixels_.size(), (size_t)s * s * 4);
        EXPECT_GT(alphaAt(r.value(), s/2, s/2), 200) << "center opaque at " << s;
    }
}

} // namespace
