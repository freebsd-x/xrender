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

namespace {

// One dimension of the exact squared Euclidean distance transform (Felzenszwalb and
// Huttenlocher): f in, d out, n samples, with scratch v and z.
void edt1d(const float* f, float* d, int n, int* v, float* z) {
    constexpr float kInf = 1e20f;
    const auto meet = [&](int q, int r) {   // where the parabolas from q and r cross
        return ((f[q] + static_cast<float>(q) * q) - (f[r] + static_cast<float>(r) * r)) / (2.0f * static_cast<float>(q - r));
    };
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        float s = meet(q, v[k]);
        while (k > 0 && s <= z[k]) s = meet(q, v[--k]);
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        const float dq = static_cast<float>(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// Squared distance from each cell to the nearest cell where `seed` is set.
void edt2d(const std::vector<uint8_t>& seed, int w, int h, std::vector<float>& out) {
    constexpr float kInf = 1e20f;
    out.assign(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < out.size(); ++i) out[i] = seed[i] ? 0 : kInf;
    const int n = std::max(w, h);
    std::vector<float> f(n), d(n), z(n + 1);
    std::vector<int> v(n);
    for (int x = 0; x < w; ++x) {
        bool any = false;
        for (int y = 0; y < h; ++y) {
            f[y] = out[static_cast<size_t>(y) * w + x];
            any = any || f[y] == 0;
        }
        if (!any) continue;   // no seed in this column: it stays at "far"
        edt1d(f.data(), d.data(), h, v.data(), z.data());
        for (int y = 0; y < h; ++y) out[static_cast<size_t>(y) * w + x] = d[y];
    }
    for (int y = 0; y < h; ++y) {
        float* row = out.data() + static_cast<size_t>(y) * w;
        if (std::all_of(row, row + w, [](float v) { return v >= kInf; })) continue;
        std::copy(row, row + w, f.begin());
        edt1d(f.data(), d.data(), w, v.data(), z.data());
        std::copy(d.begin(), d.begin() + w, row);
    }
}

}  // namespace

// The glyph's coverage, upsampled kUp times (bilinear) and cut at half, places its edges
// to a fraction of a pixel; exact distance transforms of that, inside and out, sampled
// at each output pixel's centre, give a field whose edges are the curves', not stair
// steps of whole reference pixels.
std::vector<uint8_t> GlyphCache::computeSdf(const FT_Bitmap* bitmap,
                                              int spread) const noexcept {
    const int src_w = static_cast<int>(bitmap->width);
    const int src_h = static_cast<int>(bitmap->rows);
    const int out_w = src_w + spread * 2;
    const int out_h = src_h + spread * 2;
    std::vector<uint8_t> out(static_cast<size_t>(out_w * out_h), 0u);
    if (src_w == 0 || src_h == 0) return out;

    const bool bgra = bitmap->pixel_mode == FT_PIXEL_MODE_BGRA;
    const bool mono = bitmap->pixel_mode == FT_PIXEL_MODE_MONO;
    // Coverage with a pixel of nothing around it, so sampling needs no bounds checks.
    const int cw = src_w + 2, ch = src_h + 2;
    std::vector<float> cov(static_cast<size_t>(cw) * ch, 0.0f);
    for (int y = 0; y < src_h; ++y) {
        const uint8_t* row = bitmap->buffer + static_cast<ptrdiff_t>(y) * bitmap->pitch;
        for (int x = 0; x < src_w; ++x)
            cov[static_cast<size_t>(y + 1) * cw + x + 1] =
                mono ? ((row[x >> 3] & (0x80 >> (x & 7))) ? 1.0f : 0.0f) : (bgra ? row[x * 4 + 3] : row[x]) / 255.0f;
    }
    const auto coverage = [&](int x, int y) -> float {
        if (x < -1 || x > src_w || y < -1 || y > src_h) return 0.0f;
        return cov[static_cast<size_t>(y + 1) * cw + x + 1];
    };

    constexpr int kUp = 3;   // ~0.08 px from the outline; 4 is barely closer and half again slower
    const int hw = out_w * kUp, hh = out_h * kUp;
    // Fine cell centres in source pixel coordinates, between pixel centres.
    const auto fine = [&](int i) { return (static_cast<float>(i) + 0.5f) / kUp - static_cast<float>(spread) - 0.5f; };
    // Each source row upsampled across once; then each fine row is a mix of two of them.
    std::vector<float> across(static_cast<size_t>(ch) * hw);
    std::vector<int> col0(hw);
    std::vector<float> colf(hw);
    for (int x = 0; x < hw; ++x) {
        const float sx = fine(x);
        col0[x] = static_cast<int>(std::floor(sx));
        colf[x] = sx - static_cast<float>(col0[x]);
    }
    for (int sy = -1; sy <= src_h; ++sy) {
        float* dst = across.data() + static_cast<size_t>(sy + 1) * hw;
        for (int x = 0; x < hw; ++x) dst[x] = coverage(col0[x], sy) * (1 - colf[x]) + coverage(col0[x] + 1, sy) * colf[x];
    }
    std::vector<uint8_t> inside(static_cast<size_t>(hw) * hh);
    for (int y = 0; y < hh; ++y) {
        const float sy = fine(y);
        const int y0 = static_cast<int>(std::floor(sy));
        const float fy = sy - static_cast<float>(y0);
        uint8_t* row = inside.data() + static_cast<size_t>(y) * hw;
        if (y0 + 1 < -1 || y0 > src_h) { std::fill(row, row + hw, 0); continue; }
        const float* a0 = y0 >= -1 ? across.data() + static_cast<size_t>(y0 + 1) * hw : nullptr;
        const float* a1 = y0 + 1 <= src_h ? across.data() + static_cast<size_t>(y0 + 2) * hw : nullptr;
        for (int x = 0; x < hw; ++x) {
            const float c = (a0 ? a0[x] : 0.0f) * (1 - fy) + (a1 ? a1[x] : 0.0f) * fy;
            row[x] = c >= 0.5f;
        }
    }
    // The boundary: cells next to one of the other side. One transform from them; the
    // side gives the sign. The edge runs between a boundary cell and its neighbour.
    std::vector<uint8_t> boundary(inside.size(), 0);
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < hw; ++x) {
            const uint8_t v = inside[static_cast<size_t>(y) * hw + x];
            const bool edge = (x > 0 && inside[static_cast<size_t>(y) * hw + x - 1] != v) ||
                              (x + 1 < hw && inside[static_cast<size_t>(y) * hw + x + 1] != v) ||
                              (y > 0 && inside[static_cast<size_t>(y - 1) * hw + x] != v) ||
                              (y + 1 < hh && inside[static_cast<size_t>(y + 1) * hw + x] != v);
            boundary[static_cast<size_t>(y) * hw + x] = edge;
        }
    std::vector<float> to_edge;
    edt2d(boundary, hw, hh, to_edge);

    // Each output pixel: the fine cells nearest its centre (two each way when kUp is even).
    const int c0 = (kUp - 1) / 2, c1 = kUp / 2;
    const float taps = static_cast<float>((c1 - c0 + 1) * (c1 - c0 + 1));
    for (int oy = 0; oy < out_h; ++oy) {
        for (int ox = 0; ox < out_w; ++ox) {
            float sum = 0;
            for (int dy = c0; dy <= c1; ++dy)
                for (int dx = c0; dx <= c1; ++dx) {
                    const size_t i = static_cast<size_t>(oy * kUp + dy) * hw + static_cast<size_t>(ox * kUp + dx);
                    const float d = std::sqrt(to_edge[i]) + 0.5f;   // to the edge beyond the nearest boundary cell
                    sum += inside[i] ? d : -d;
                }
            const float dist = (sum / taps) / kUp;   // in output pixels, positive inside
            const float normalised = (dist / static_cast<float>(spread)) * 0.5f + 0.5f;
            out[static_cast<size_t>(oy) * out_w + ox] =
                static_cast<uint8_t>(std::clamp(normalised * 255.0f, 0.0f, 255.0f));
        }
    }
    return out;
}

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

    if (FT_Load_Glyph(ft_face, glyph_id, FT_LOAD_DEFAULT)) {
        // Mark as failed so we don't retry on every frame.
        metrics_[{face_id, glyph_id}] = GlyphMetric{};
        return true;
    }

    // Rendered antialiased, then measured (computeSdf): FreeType's own outline SDF is
    // smoother still but several times slower, and glyphs are made on first use.
    if (FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL) && ft_face->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
        metrics_[{face_id, glyph_id}] = GlyphMetric{};
        return true;
    }
    FT_Bitmap* bm = &ft_face->glyph->bitmap;
    const uint32_t sdfW = bm->width + static_cast<uint32_t>(kSdfSpread * 2);
    const uint32_t sdfH = bm->rows  + static_cast<uint32_t>(kSdfSpread * 2);
    const std::vector<uint8_t> sdf = computeSdf(bm, kSdfSpread);
    const float bearing_x = static_cast<float>(ft_face->glyph->bitmap_left) - static_cast<float>(kSdfSpread);
    const float bearing_y = static_cast<float>(ft_face->glyph->bitmap_top) + static_cast<float>(kSdfSpread);

    uint32_t tx, ty, tp;
    if (!allocateTile(sdfW, sdfH, tx, ty, tp)) return false;

    // Blit SDF into the page bitmap.
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
        .bearing_x_ = bearing_x,
        .bearing_y_ = bearing_y,
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
