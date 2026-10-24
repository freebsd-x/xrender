#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <system_error>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

namespace fbx::xrender {

class GlyphCache;

// ── Font description types ────────────────────────────────────────────────────

// CSS-compatible weight values (100–900).
enum class FontWeight : uint16_t {
    Thin       = 100,
    ExtraLight = 200,
    Light      = 300,
    Regular    = 400,
    Medium     = 500,
    SemiBold   = 600,
    Bold       = 700,
    ExtraBold  = 800,
    Black      = 900,
};

enum class FontStyle : uint8_t {
    Normal  = 0,
    Italic  = 1,
    Oblique = 2,
};

// A loaded font face pairing FreeType and HarfBuzz.
// Owned and cached by FontManager.
struct FontFace {
    FT_Library ft_lib_{nullptr};
    FT_Face    ft_face_{nullptr};
    hb_font_t* hb_font_{nullptr};
    uint32_t   face_id_{0}; // Unique ID assigned by FontManager (never 0)

    FontFace() noexcept = default;
    ~FontFace() noexcept;

    FontFace(const FontFace&)            = delete;
    FontFace& operator=(const FontFace&) = delete;
    FontFace(FontFace&&) noexcept;
    FontFace& operator=(FontFace&&) noexcept;

    [[nodiscard]] bool valid() const noexcept { return ft_face_ != nullptr; }
};

// ── FontManager ───────────────────────────────────────────────────────────────

class FontManager {
public:
    static void init() noexcept;    // Call once at startup (initialises Fontconfig).
    static void cleanup() noexcept; // Call once at shutdown.

    // Look up (or load) a font face matching the given description.
    // An empty family string means "default UI font" (the system sans-serif).
    // Returns nullptr if the font cannot be found or loaded.
    [[nodiscard]] static FontFace* getFace(
        const std::string& family = "",
        FontWeight weight = FontWeight::Regular,
        FontStyle  style  = FontStyle::Normal
    ) noexcept;

    // Returns the emoji fallback face (Noto Color Emoji / similar), or nullptr.
    [[nodiscard]] static FontFace* getEmojiFace() noexcept;

    // Pre-warm glyph cache: shape utf8_text with face and ensure every resulting
    // glyph is present in cache.  Call this before rendering each text string.
    static void ensureGlyphs(const std::string& utf8_text,
                              FontFace*          face,
                              GlyphCache&        cache) noexcept;

    // Measure the advance width of utf8_text at font_size pixels.
    [[nodiscard]] static float measureText(const std::string& utf8_text,
                                           float fontSize) noexcept;
    [[nodiscard]] static float measureText(const std::string& utf8_text,
                                           float              fontSize,
                                           const std::string& family,
                                           FontWeight         weight,
                                           FontStyle          style) noexcept;

    // Legacy compatibility: supply atlas+shaper pointers for measureText fallback.
    static void setTextProvider(const void* atlas, void* shaper) noexcept;

    // Resolve a font file path via Fontconfig.
    [[nodiscard]] static std::expected<std::string, std::error_code>
    findFontPath(const std::string& family,
                 bool bold   = false,
                 bool italic = false) noexcept;
};

} // namespace fbx::xrender
