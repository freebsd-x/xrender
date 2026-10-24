#include "xrender/font_atlas.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace fbx::xrender {

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<GlyphCache> GlyphCache::create() noexcept {
    auto cache = std::unique_ptr<GlyphCache>(new GlyphCache());
    cache->addPage();
    return cache;
}

// ── Page management ───────────────────────────────────────────────────────────

void GlyphCache::addPage() noexcept {
    AtlasPage pg;
    pg.bitmap.assign(kPageSize * kPageSize, 0u);
    pg.cursor_x  = 1;
    pg.cursor_y  = 1;
    pg.row_height = 0;
    pages_.push_back(std::move(pg));
}

bool GlyphCache::allocateTile(uint32_t w, uint32_t h,
                               uint32_t& out_x, uint32_t& out_y,
                               uint32_t& out_page) noexcept {
    // Try fitting in an existing page.
    for (uint32_t pi = 0; pi < static_cast<uint32_t>(pages_.size()); ++pi) {
        AtlasPage& pg = pages_[pi];

        uint32_t x = pg.cursor_x;
        uint32_t y = pg.cursor_y;

        if (x + w + 1 >= kPageSize) {
            // Wrap to next row.
            x = 1;
            y = pg.cursor_y + pg.row_height + 1;
            pg.row_height = 0;
        }

        if (y + h + 1 >= kPageSize) continue; // This page is full.

        if (h > pg.row_height) pg.row_height = h;
        pg.cursor_x = x + w + 1;
        pg.cursor_y = y;

        out_x = x; out_y = y; out_page = pi;
        return true;
    }

    // All existing pages are full — add a new one.
    if (w + 2 >= kPageSize || h + 2 >= kPageSize) {
        std::cerr << "[xrender] GlyphCache: glyph too large for a single page.\n";
        return false;
    }

    addPage();
    uint32_t pi = static_cast<uint32_t>(pages_.size()) - 1;
    AtlasPage& pg = pages_[pi];
    out_x = pg.cursor_x;
    out_y = pg.cursor_y;
    out_page = pi;
    pg.row_height = h;
    pg.cursor_x += w + 1;
    return true;
}

// ── SDF computation ───────────────────────────────────────────────────────────

std::vector<uint8_t> GlyphCache::computeSdf(const FT_Bitmap* bitmap,
                                              int spread) const noexcept {
    int src_w = static_cast<int>(bitmap->width);
    int src_h = static_cast<int>(bitmap->rows);
    int out_w = src_w + spread * 2;
    int out_h = src_h + spread * 2;
    std::vector<uint8_t> out(static_cast<size_t>(out_w * out_h), 0u);

    if (src_w == 0 || src_h == 0) return out;

    auto is_filled = [bitmap, src_w, src_h](int x, int y) -> bool {
        if (x < 0 || x >= src_w || y < 0 || y >= src_h) return false;
        return bitmap->buffer[y * bitmap->pitch + x] > 127;
    };

    for (int oy = 0; oy < out_h; ++oy) {
        for (int ox = 0; ox < out_w; ++ox) {
            int sx = ox - spread;
            int sy = oy - spread;
            bool inside = is_filled(sx, sy);
            float min_dist_sq = static_cast<float>(spread * spread + 1);

            for (int dy = -spread; dy <= spread; ++dy) {
                for (int dx = -spread; dx <= spread; ++dx) {
                    if (is_filled(sx + dx, sy + dy) != inside) {
                        float d = static_cast<float>(dx * dx + dy * dy);
                        if (d < min_dist_sq) min_dist_sq = d;
                    }
                }
            }

            float dist = std::sqrt(min_dist_sq);
            float signed_dist = inside ? dist : -dist;
            float normalised = (signed_dist / static_cast<float>(spread)) * 0.5f + 0.5f;
            out[oy * out_w + ox] =
                static_cast<uint8_t>(std::clamp(normalised * 255.0f, 0.0f, 255.0f));
        }
    }
    return out;
}

// ── Public API ────────────────────────────────────────────────────────────────

const GlyphMetric* GlyphCache::lookup(uint32_t face_id,
                                       uint32_t glyph_id) const noexcept {
    auto it = metrics_.find({face_id, glyph_id});
    if (it != metrics_.end()) return &it->second;
    return nullptr;
}

bool GlyphCache::addGlyph(FT_Face ft_face,
                           uint32_t face_id,
                           uint32_t glyph_id) noexcept {
    // Already cached (even if it was an empty/missing glyph).
    if (metrics_.count({face_id, glyph_id})) return true;

    FT_Set_Pixel_Sizes(ft_face, 0, static_cast<FT_UInt>(kRefSize));

    if (FT_Load_Glyph(ft_face, glyph_id, FT_LOAD_RENDER)) {
        // Mark as failed so we don't retry on every frame.
        metrics_[{face_id, glyph_id}] = GlyphMetric{};
        return true;
    }

    FT_Bitmap* bm   = &ft_face->glyph->bitmap;
    uint32_t   sdfW = bm->width + static_cast<uint32_t>(kSdfSpread * 2);
    uint32_t   sdfH = bm->rows  + static_cast<uint32_t>(kSdfSpread * 2);

    uint32_t tx, ty, tp;
    if (!allocateTile(sdfW, sdfH, tx, ty, tp)) return false;

    // Blit SDF into the page bitmap.
    auto sdf = computeSdf(bm, kSdfSpread);
    auto& pg = pages_[tp];
    for (uint32_t row = 0; row < sdfH; ++row) {
        for (uint32_t col = 0; col < sdfW; ++col) {
            pg.bitmap[(ty + row) * kPageSize + (tx + col)] =
                sdf[row * sdfW + col];
        }
    }

    // Store metrics (normalised to page size).
    float ps = static_cast<float>(kPageSize);
    metrics_[{face_id, glyph_id}] = GlyphMetric{
        .advance_x_ = static_cast<float>(ft_face->glyph->advance.x >> 6),
        .bearing_x_ = static_cast<float>(ft_face->glyph->bitmap_left) -
                      static_cast<float>(kSdfSpread),
        .bearing_y_ = static_cast<float>(ft_face->glyph->bitmap_top) +
                      static_cast<float>(kSdfSpread),
        .width_    = static_cast<float>(sdfW),
        .height_   = static_cast<float>(sdfH),
        .atlas_u_  = static_cast<float>(tx)        / ps,
        .atlas_v_  = static_cast<float>(ty)        / ps,
        .atlas_u2_ = static_cast<float>(tx + sdfW) / ps,
        .atlas_v2_ = static_cast<float>(ty + sdfH) / ps,
        .page_     = tp
    };

    dirty_ = true;
    return true;
}

const std::vector<uint8_t>& GlyphCache::getPageBitmap(uint32_t page) const noexcept {
    static const std::vector<uint8_t> kEmpty;
    if (page >= pages_.size()) return kEmpty;
    return pages_[page].bitmap;
}

} // namespace fbx::xrender
