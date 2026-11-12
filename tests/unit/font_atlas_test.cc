// The glyph atlas's distance fields, against FreeType's own (measured to the outline
// itself): close to it, and quick to make.

#include <gtest/gtest.h>

#include <algorithm>
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

TEST(FontAtlas, GlyphsNeverShareSpace) {
    // A Korean page fills more than a page of the atlas. The row height used to be lost
    // when a page turned out full, and later glyphs were packed over earlier ones.
    const char* path = "/usr/local/share/fonts/nanum/NanumGothic.ttf";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "no CJK font";
    FT_Library lib;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);
    FT_Face face;
    ASSERT_EQ(FT_New_Face(lib, path, 0, &face), 0);
    auto cache = GlyphCache::create();
    ASSERT_TRUE(cache);
    std::vector<uint32_t> ids;
    for (uint32_t g = 1; g < static_cast<uint32_t>(face->num_glyphs) && cache->getPageCount() < 3; ++g) {
        ASSERT_TRUE(cache->addGlyph(face, 1, g));
        ids.push_back(g);
    }
    EXPECT_GE(cache->getPageCount(), 2u);
    struct Box { uint32_t page; float x0, y0, x1, y1; };
    std::vector<Box> boxes;
    for (uint32_t g : ids) {
        const GlyphMetric* m = cache->lookup(1, g);
        ASSERT_NE(m, nullptr);
        if (m->width_ == 0) continue;
        boxes.push_back({m->page_, m->atlas_u_, m->atlas_v_, m->atlas_u2_, m->atlas_v2_});
    }
    // Sweep by page and left edge: neighbours only.
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.page != b.page ? a.page < b.page : a.y0 < b.y0; });
    size_t overlaps = 0;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size() && boxes[j].page == boxes[i].page && boxes[j].y0 < boxes[i].y1; ++j)
            if (boxes[j].x0 < boxes[i].x1 && boxes[i].x0 < boxes[j].x1 && boxes[j].y0 < boxes[i].y1 && boxes[i].y0 < boxes[j].y1) ++overlaps;
    EXPECT_EQ(overlaps, 0u);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}
