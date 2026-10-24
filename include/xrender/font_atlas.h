#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace fbx::xrender {

enum class FontError {
    LibraryInitFailed,
    FontFaceLoadFailed,
    GlyphLoadFailed,
    AtlasGenerationFailed
};

// Per-glyph metrics in the atlas (all sizes in atlas-reference pixels, i.e. kRefSize).
struct GlyphMetric {
    float advance_x_; // Horizontal advance
    float bearing_x_; // Left bearing (may be negative)
    float bearing_y_; // Top bearing from baseline (positive = above baseline)
    float width_;     // Glyph quad width  (including SDF padding)
    float height_;    // Glyph quad height (including SDF padding)
    float atlas_u_;   // Top-left U  (normalised [0..1])
    float atlas_v_;   // Top-left V  (normalised [0..1])
    float atlas_u2_;  // Bottom-right U
    float atlas_v2_;  // Bottom-right V
    uint32_t page_;   // Atlas page index (for multi-page support)
};

// Key that uniquely identifies a glyph across all loaded faces.
struct GlyphKey {
    uint32_t face_id;   // Assigned by FontManager; 0 = invalid
    uint32_t glyph_id;  // HarfBuzz / FreeType glyph index within the face
    bool operator==(const GlyphKey&) const noexcept = default;
};

struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& k) const noexcept {
        return std::hash<uint64_t>{}(
            (static_cast<uint64_t>(k.face_id) << 32) | k.glyph_id);
    }
};

// Dynamic, multi-page SDF glyph cache.
// Glyphs are rasterized on demand via addGlyph() and packed into 2048×2048 pages.
// New pages are allocated automatically when the current page is full.
// The dirty flag is set whenever new glyphs are added; callers should re-upload
// the changed pages to GPU and then call clearDirty().
class GlyphCache {
public:
    static constexpr uint32_t kPageSize  = 2048; // Width and height of each atlas page
    static constexpr float    kRefSize   = 48.0f; // FreeType pixel size for rasterisation
    static constexpr int      kSdfSpread = 4;     // SDF padding in pixels

    [[nodiscard]] static std::unique_ptr<GlyphCache> create() noexcept;

    // Look up a previously added glyph.  Returns nullptr if not yet cached.
    [[nodiscard]] const GlyphMetric* lookup(uint32_t face_id, uint32_t glyph_id) const noexcept;

    // Rasterize a glyph from ft_face (sized at kRefSize) and insert it.
    // Returns false only if the atlas is catastrophically full (very rare).
    bool addGlyph(FT_Face ft_face, uint32_t face_id, uint32_t glyph_id) noexcept;

    [[nodiscard]] bool     isDirty()    const noexcept { return dirty_; }
    void                   clearDirty() noexcept       { dirty_ = false; }

    [[nodiscard]] const std::vector<uint8_t>& getPageBitmap(uint32_t page) const noexcept;
    [[nodiscard]] uint32_t getPageCount() const noexcept {
        return static_cast<uint32_t>(pages_.size());
    }

private:
    GlyphCache() noexcept = default;

    struct AtlasPage {
        std::vector<uint8_t> bitmap;
        uint32_t cursor_x{1};    // Next tile X (start at 1 to avoid UV=0 ambiguity)
        uint32_t cursor_y{1};    // Next tile Y
        uint32_t row_height{0};  // Tallest glyph in the current row
    };

    std::vector<AtlasPage> pages_;
    std::unordered_map<GlyphKey, GlyphMetric, GlyphKeyHash> metrics_;
    bool dirty_{false};

    void addPage() noexcept;
    bool allocateTile(uint32_t w, uint32_t h,
                      uint32_t& out_x, uint32_t& out_y, uint32_t& out_page) noexcept;
    [[nodiscard]] std::vector<uint8_t> computeSdf(const FT_Bitmap* bitmap,
                                                   int spread) const noexcept;
};

// Legacy alias — TypographyBackend still refers to FontAtlas in some headers.
using FontAtlas = GlyphCache;

} // namespace fbx::xrender
