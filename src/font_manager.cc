#include "xrender/font_manager.h"
#include "xrender/font_atlas.h"
#include "xrender/text_shaper.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include <hb.h>
#include <hb-ft.h>

#include <atomic>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace fbx::xrender {

// ── FontFace lifecycle ────────────────────────────────────────────────────────

FontFace::~FontFace() noexcept {
    if (hb_font_) { hb_font_destroy(hb_font_); hb_font_ = nullptr; }
    if (ft_face_) { FT_Done_Face(ft_face_);    ft_face_ = nullptr; }
    if (ft_lib_)  { FT_Done_FreeType(ft_lib_); ft_lib_  = nullptr; }
}

FontFace::FontFace(FontFace&& o) noexcept
    : ft_lib_(std::exchange(o.ft_lib_, nullptr)),
      ft_face_(std::exchange(o.ft_face_, nullptr)),
      hb_font_(std::exchange(o.hb_font_, nullptr)),
      face_id_(std::exchange(o.face_id_, 0u)) {}

FontFace& FontFace::operator=(FontFace&& o) noexcept {
    if (this != &o) {
        if (hb_font_) hb_font_destroy(hb_font_);
        if (ft_face_) FT_Done_Face(ft_face_);
        if (ft_lib_)  FT_Done_FreeType(ft_lib_);
        ft_lib_  = std::exchange(o.ft_lib_, nullptr);
        ft_face_ = std::exchange(o.ft_face_, nullptr);
        hb_font_ = std::exchange(o.hb_font_, nullptr);
        face_id_ = std::exchange(o.face_id_, 0u);
    }
    return *this;
}

// ── Internal state ────────────────────────────────────────────────────────────

namespace {

struct FaceKey {
    std::string family;
    FontWeight  weight;
    FontStyle   style;
    bool operator==(const FaceKey&) const noexcept = default;
};

struct FaceKeyHash {
    std::size_t operator()(const FaceKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.family);
        h ^= std::hash<uint16_t>{}(static_cast<uint16_t>(k.weight)) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.style))   + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

std::unordered_map<FaceKey, std::unique_ptr<FontFace>, FaceKeyHash> g_face_cache;
std::unique_ptr<FontFace> g_emoji_face;
std::unique_ptr<FontFace> g_last_resort_face;
bool g_last_resort_tried = false;
std::mutex                g_mutex;
std::atomic<uint32_t>     g_next_face_id{1};
bool                      g_fc_ready{false};

// ── Internal helpers ──────────────────────────────────────────────────────────

// Map FontWeight → Fontconfig FC_WEIGHT constant (roughly).
int weightToFcWeight(FontWeight w) noexcept {
    switch (w) {
    case FontWeight::Thin:       return FC_WEIGHT_THIN;
    case FontWeight::ExtraLight: return FC_WEIGHT_EXTRALIGHT;
    case FontWeight::Light:      return FC_WEIGHT_LIGHT;
    case FontWeight::Regular:    return FC_WEIGHT_REGULAR;
    case FontWeight::Medium:     return FC_WEIGHT_MEDIUM;
    case FontWeight::SemiBold:   return FC_WEIGHT_DEMIBOLD;
    case FontWeight::Bold:       return FC_WEIGHT_BOLD;
    case FontWeight::ExtraBold:  return FC_WEIGHT_EXTRABOLD;
    case FontWeight::Black:      return FC_WEIGHT_BLACK;
    }
    return FC_WEIGHT_REGULAR;
}

std::string resolveViaFontconfig(const std::string& family,
                                  FontWeight         weight,
                                  FontStyle          style) noexcept {
    FcPattern* pat = FcPatternCreate();
    if (!pat) return {};

    if (!family.empty())
        FcPatternAddString(pat, FC_FAMILY,
                           reinterpret_cast<const FcChar8*>(family.c_str()));

    FcPatternAddInteger(pat, FC_WEIGHT, weightToFcWeight(weight));

    if (style == FontStyle::Italic)
        FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
    else if (style == FontStyle::Oblique)
        FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_OBLIQUE);
    else
        FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);

    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult   result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);
    if (!match) return {};

    FcChar8* file = nullptr;
    std::string path;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
        path = reinterpret_cast<const char*>(file);
    FcPatternDestroy(match);
    return path;
}

// Try to set a variable font weight axis so we get intermediate weights
// (e.g. Medium=500) from a single variable font file.
void applyVariableWeight(FT_Library lib, FT_Face face,
                          hb_font_t* hbf, FontWeight weight) noexcept {
    if (!FT_HAS_MULTIPLE_MASTERS(face)) return;

    FT_MM_Var* mm = nullptr;
    if (FT_Get_MM_Var(face, &mm)) return;

    std::vector<FT_Fixed> coords(mm->num_axis);
    FT_Get_Var_Design_Coordinates(face, mm->num_axis, coords.data());

    bool changed = false;
    for (FT_UInt i = 0; i < mm->num_axis; ++i) {
        if (mm->axis[i].tag == FT_MAKE_TAG('w','g','h','t')) {
            FT_Fixed req = static_cast<FT_Fixed>(static_cast<uint16_t>(weight)) << 16;
            // Clamp to axis range.
            req = std::max(mm->axis[i].minimum, std::min(mm->axis[i].maximum, req));
            coords[i] = req;
            changed = true;
        }
    }
    FT_Done_MM_Var(lib, mm);

    if (changed) {
        FT_Set_Var_Design_Coordinates(face, static_cast<FT_UInt>(coords.size()),
                                       coords.data());
        // Sync HarfBuzz to the new coordinates.
        hb_ft_font_changed(hbf);
    }
}

std::unique_ptr<FontFace> loadFace(const std::string& path,
                                    FontWeight         weight,
                                    FontStyle          /*style*/) noexcept {
    auto f = std::make_unique<FontFace>();
    f->face_id_ = g_next_face_id.fetch_add(1, std::memory_order_relaxed);

    if (FT_Init_FreeType(&f->ft_lib_)) {
        std::cerr << "[xrender] FontManager: FT_Init_FreeType failed\n";
        return nullptr;
    }
    if (FT_New_Face(f->ft_lib_, path.c_str(), 0, &f->ft_face_)) {
        std::cerr << "[xrender] FontManager: failed to load face: " << path << "\n";
        return nullptr;
    }
    FT_Set_Pixel_Sizes(f->ft_face_, 0, static_cast<FT_UInt>(GlyphCache::kRefSize));

    f->hb_font_ = hb_ft_font_create(f->ft_face_, nullptr);
    hb_ft_font_set_load_flags(f->hb_font_, FT_LOAD_DEFAULT);

    applyVariableWeight(f->ft_lib_, f->ft_face_, f->hb_font_, weight);

    return f;
}

} // anonymous namespace

// ── FontManager public API ────────────────────────────────────────────────────

void FontManager::init() noexcept {
    FcInit();
    g_fc_ready = true;
    std::cout << "[xrender] FontManager: Fontconfig ready.\n";
}

void FontManager::cleanup() noexcept {
    std::lock_guard lk(g_mutex);
    g_face_cache.clear();
    g_emoji_face.reset();
    g_last_resort_face.reset();
    g_last_resort_tried = false;
    FcFini();
}

FontFace* FontManager::getFace(const std::string& family,
                                FontWeight         weight,
                                FontStyle          style) noexcept {
    FaceKey key{family, weight, style};

    {
        std::lock_guard lk(g_mutex);
        auto it = g_face_cache.find(key);
        if (it != g_face_cache.end()) return it->second.get();
    }

    std::string path = resolveViaFontconfig(family, weight, style);
    if (path.empty()) {
        std::cerr << "[xrender] FontManager: no font for family='"
                  << family << "'\n";
        return nullptr;
    }

    auto face = loadFace(path, weight, style);
    if (!face) return nullptr;

    std::lock_guard lk(g_mutex);
    auto [it, ok] = g_face_cache.emplace(key, std::move(face));
    return it->second.get();
}

FontFace* FontManager::getEmojiFace() noexcept {
    {
        std::lock_guard lk(g_mutex);
        if (g_emoji_face) return g_emoji_face.get();
    }

    // Try common emoji font families in priority order. The color-emoji fonts
    // come first; the monochrome Noto Sans Symbols fonts are the fallback that is
    // actually installed here and whose glyphs work with the SDF atlas.
    static const char* kEmojiFamilies[] = {
        "Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji",
        "Twemoji", "Noto Emoji",
        "Noto Sans Symbols 2", "Noto Sans Symbols", nullptr
    };

    for (int i = 0; kEmojiFamilies[i]; ++i) {
        std::string path = resolveViaFontconfig(
            kEmojiFamilies[i], FontWeight::Regular, FontStyle::Normal);
        if (path.empty()) continue;

        auto face = loadFace(path, FontWeight::Regular, FontStyle::Normal);
        if (!face) continue;

        // Fontconfig substitutes a non-emoji font (e.g. Noto Sans) when the
        // requested family is absent, so a non-empty path does not mean the face
        // covers emoji. Require a representative glyph (U+1F50A 🔊) before
        // accepting it, otherwise keep looking down the list.
        if (face->ft_face_ && FT_Get_Char_Index(face->ft_face_, 0x1F50A) == 0)
            continue;

        std::lock_guard lk(g_mutex);
        g_emoji_face = std::move(face);
        std::cout << "[xrender] FontManager: emoji font loaded: " << path << "\n";
        return g_emoji_face.get();
    }

    std::cerr << "[xrender] FontManager: no emoji font found on system.\n";
    return nullptr;
}

FontFace* FontManager::getLastResortFace() noexcept {
    {
        std::lock_guard lk(g_mutex);
        if (g_last_resort_tried) return g_last_resort_face.get();
    }
    std::unique_ptr<FontFace> found;
    for (const char* family : {"DejaVu Sans", "DejaVu Sans Mono"}) {
        std::string path = resolveViaFontconfig(family, FontWeight::Regular, FontStyle::Normal);
        if (path.empty()) continue;
        auto face = loadFace(path, FontWeight::Regular, FontStyle::Normal);
        // Fontconfig substitutes when a family is absent: require an arrow (←).
        if (face && face->ft_face_ && FT_Get_Char_Index(face->ft_face_, 0x2190) != 0) {
            found = std::move(face);
            break;
        }
    }
    std::lock_guard lk(g_mutex);
    if (!g_last_resort_tried) {
        g_last_resort_tried = true;
        g_last_resort_face = std::move(found);
    }
    return g_last_resort_face.get();
}

FontFace* FontManager::faceForId(uint32_t face_id, FontFace* primary) noexcept {
    if (FontFace* e = getEmojiFace(); e && e->face_id_ == face_id) return e;
    if (FontFace* l = getLastResortFace(); l && l->face_id_ == face_id) return l;
    return primary;
}

void FontManager::ensureGlyphs(const std::string& utf8_text,
                                FontFace*          face,
                                GlyphCache&        cache) noexcept {
    if (!face || utf8_text.empty()) return;

    // Shape with the primary face AND the emoji fallback so codepoints the
    // primary face lacks (🔊 ⌨ 🔋 …) are re-shaped against the emoji face and
    // their glyphs get added to the atlas under the emoji face id.
    FontFace* emoji = getEmojiFace();
    auto& shaper = TextShaper::getInstance();
    auto  glyphs = shaper.shapeText(utf8_text,
                                     face->hb_font_, face->face_id_,
                                     emoji ? emoji->hb_font_ : nullptr,
                                     emoji ? emoji->face_id_ : 0);

    for (const auto& g : glyphs) {
        if (g.glyph_index_ == 0) continue; // uncovered by every face — nothing to add
        if (cache.lookup(g.face_id_, g.glyph_index_)) continue;
        // Rasterise each glyph from the face it was resolved against (the
        // primary, the emoji face or the last-resort face).
        FontFace* from = faceForId(g.face_id_, face);
        FT_Face ft = from ? from->ft_face_ : nullptr;
        if (ft) cache.addGlyph(ft, g.face_id_, g.glyph_index_);
    }
}

float FontManager::measureText(const std::string& utf8_text,
                                float              fontSize) noexcept {
    FontFace* face = getFace("", FontWeight::Regular, FontStyle::Normal);
    if (!face) return static_cast<float>(utf8_text.size()) * fontSize * 0.5f;
    return measureText(utf8_text, fontSize, "", FontWeight::Regular, FontStyle::Normal);
}

float FontManager::measureText(const std::string& utf8_text,
                                float              fontSize,
                                const std::string& family,
                                FontWeight         weight,
                                FontStyle          style) noexcept {
    if (utf8_text.empty()) return 0.0f;

    FontFace* face = getFace(family, weight, style);
    if (!face) return static_cast<float>(utf8_text.size()) * fontSize * 0.5f;

    auto& shaper = TextShaper::getInstance();
    // Measure with the same fallbacks text is drawn with: a glyph the face
    // lacks (←, ⏏) used to measure as nothing, so its button collapsed.
    FontFace* emoji = getEmojiFace();
    auto  glyphs = shaper.shapeText(utf8_text, face->hb_font_, face->face_id_,
                                     emoji ? emoji->hb_font_ : nullptr,
                                     emoji ? emoji->face_id_ : 0);

    float scale = fontSize / GlyphCache::kRefSize;
    float total = 0.0f;
    for (const auto& g : glyphs) total += g.x_advance_ * scale;
    return total;
}

void FontManager::setTextProvider(const void* /*atlas*/, void* /*shaper*/) noexcept {
    // No-op: legacy hook kept for binary compatibility.
}

std::expected<std::string, std::error_code>
FontManager::findFontPath(const std::string& family,
                           bool bold, bool italic) noexcept {
    std::string path = resolveViaFontconfig(
        family,
        bold   ? FontWeight::Bold   : FontWeight::Regular,
        italic ? FontStyle::Italic  : FontStyle::Normal
    );
    if (path.empty())
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    return path;
}

} // namespace fbx::xrender
