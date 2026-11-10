// The glyph atlas's distance fields, against FreeType's own (measured to the outline
// itself): close to it, and quick to make.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>

#include "xrender/font_atlas.h"

#include FT_MODULE_H

using namespace fbx::xrender;

namespace {

const char* findFont() {
    for (const char* p : {"/usr/local/share/fonts/noto/NotoSans-Regular.ttf", "/usr/local/share/fonts/dejavu/DejaVuSans.ttf"})
        if (std::filesystem::exists(p)) return p;
    return nullptr;
}

}  // namespace

TEST(FontAtlas, DistanceFieldsFollowTheOutline) {
    const char* path = findFont();
    if (!path) GTEST_SKIP() << "no test font";
    FT_Library lib;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);
    FT_Face face;
    ASSERT_EQ(FT_New_Face(lib, path, 0, &face), 0);
    auto cache = GlyphCache::create();
    for (const char ch : {'o', 'R'}) {
        const FT_UInt g = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        ASSERT_TRUE(cache->addGlyph(face, 1, g));
        const GlyphMetric* m = cache->lookup(1, g);
        ASSERT_TRUE(m && m->width_ > 0);
        const auto& page = cache->getPageBitmap(m->page_);
        const auto ax = static_cast<uint32_t>(std::lround(m->atlas_u_ * GlyphCache::kPageSize));
        const auto ay = static_cast<uint32_t>(std::lround(m->atlas_v_ * GlyphCache::kPageSize));

        // The truth: FreeType's outline SDF at the same size and spread.
        FT_Int spread = GlyphCache::kSdfSpread;
        FT_Property_Set(lib, "sdf", "spread", &spread);
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(GlyphCache::kRefSize));
        ASSERT_EQ(FT_Load_Glyph(face, g, FT_LOAD_DEFAULT), 0);
        ASSERT_EQ(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF), 0);
        const FT_Bitmap& t = face->glyph->bitmap;
        // Ours is placed spread pixels outside the plain bitmap; so is FreeType's.
        ASSERT_EQ(static_cast<float>(face->glyph->bitmap_left), m->bearing_x_);
        ASSERT_EQ(static_cast<float>(face->glyph->bitmap_top), m->bearing_y_);
        double err = 0;
        int n = 0;
        for (uint32_t y = 0; y < t.rows && y < static_cast<uint32_t>(m->height_); ++y)
            for (uint32_t x = 0; x < t.width && x < static_cast<uint32_t>(m->width_); ++x) {
                const int truth = t.buffer[static_cast<ptrdiff_t>(y) * t.pitch + x];
                if (truth <= 8 || truth >= 248) continue;   // clamped: beyond the spread
                const int ours = page[(ay + y) * GlyphCache::kPageSize + ax + x];
                err += std::abs(ours - truth);
                ++n;
            }
        ASSERT_GT(n, 100);
        // In pixels at the reference size: 255 levels span 2 x spread pixels.
        const double px = err / n * (2.0 * GlyphCache::kSdfSpread / 255.0);
        std::printf("%c error %.3f px\n", ch, px);
        EXPECT_LT(px, 0.15) << ch;   // a thresholded bitmap (the old way) was off by ~0.5
    }
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}

TEST(FontAtlas, MakingGlyphsIsQuick) {
    const char* path = findFont();
    if (!path) GTEST_SKIP() << "no test font";
    FT_Library lib;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);
    FT_Face face;
    ASSERT_EQ(FT_New_Face(lib, path, 0, &face), 0);
    auto cache = GlyphCache::create();
    const auto t0 = std::chrono::steady_clock::now();
    for (int ch = 33; ch < 127; ++ch) cache->addGlyph(face, 1, FT_Get_Char_Index(face, static_cast<FT_ULong>(ch)));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("94 glyphs in %.1f ms\n", ms);
    // ~30 alone; loose, for the suite runs tests side by side: it catches a slowdown many
    // times over, not noise.
    EXPECT_LT(ms, 200.0);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}
