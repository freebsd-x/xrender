#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <hb.h>

namespace fbx::xrender {

struct ShapedGlyph {
    uint32_t glyph_index_; // Glyph ID within the face that produced it
    uint32_t face_id_;     // face_id_ of the FontFace that owns this glyph
    float    x_advance_;   // Horizontal advance in reference pixels (kRefSize)
    float    y_advance_;   // Vertical advance (non-zero for vertical scripts)
    float    x_offset_;    // Horizontal kerning / positional offset
    float    y_offset_;    // Vertical   kerning / positional offset
    uint32_t cluster_;     // Byte offset into the source UTF-8 string
};

// Text shaper wrapping a re-usable HarfBuzz buffer.
// Acquire the process-wide singleton via getInstance().
class TextShaper {
public:
    TextShaper() noexcept;
    ~TextShaper() noexcept;

    TextShaper(const TextShaper&)            = delete;
    TextShaper& operator=(const TextShaper&) = delete;

    // Shape utf8_text with primary_font (primary_face_id used in ShapedGlyph.face_id_).
    // If emoji_font is non-null, any glyph with index 0 from the primary face is
    // re-shaped against emoji_font and the resulting glyph records carry emoji_face_id.
    [[nodiscard]] std::vector<ShapedGlyph>
    shapeText(const std::string& utf8_text,
              hb_font_t*         primary_font,
              uint32_t           primary_face_id,
              hb_font_t*         emoji_font    = nullptr,
              uint32_t           emoji_face_id = 0) noexcept;

    // Process-wide singleton (created on first call).
    [[nodiscard]] static TextShaper& getInstance() noexcept;

private:
    hb_buffer_t* hb_buffer_{nullptr};

    // Shape a text run with a specific font; fills result in-place.
    void shapeRun(const char* utf8, int len, int src_offset,
                  hb_font_t* font, uint32_t face_id,
                  std::vector<ShapedGlyph>& out) noexcept;
};

} // namespace fbx::xrender
