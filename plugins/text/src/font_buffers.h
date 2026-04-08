#ifndef VELK_UI_TEXT_FONT_BUFFERS_H
#define VELK_UI_TEXT_FONT_BUFFERS_H

#include "glyph_baker.h"

#include <velk/api/math_types.h>
#include <velk/vector.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <unordered_map>

namespace velk::ui {

/**
 * @brief Per-glyph metadata stored in the glyph table.
 *
 * One entry per font-internal glyph index. The shader (eventually) uses this
 * to find the glyph's curves and band data inside the font's flat buffers.
 *
 * Layout chosen to be std430-friendly: 32 bytes, vec2 members 8-byte aligned,
 * uint32_t members tail-packed with explicit padding.
 */
struct GlyphRecord
{
    /// Glyph bbox in raw FreeType font units (em coordinates). Used by layout
    /// code to size the quad and by the shader to map quad uv to glyph space.
    vec2 bbox_min;
    vec2 bbox_max;

    /// Index of the glyph's first curve inside FontBuffers::curves.
    uint32_t curve_offset;

    /// Number of curves belonging to this glyph.
    uint32_t curve_count;

    /// Offset (in uint32_t units) into FontBuffers::bands where this glyph's
    /// band data begins. The layout starting at that offset is:
    ///
    ///   [h_band_offsets[0..N]]   N + 1 uint32_t, prefix sums starting at 0
    ///   [h_curve_indices...]     h_band_offsets[N] entries of uint32_t
    ///   [v_band_offsets[0..N]]   N + 1 uint32_t
    ///   [v_curve_indices...]     v_band_offsets[N] entries
    ///
    /// where N = BakedGlyph::BAND_COUNT. Curve indices are relative to the
    /// glyph's curve list (i.e. add curve_offset to get an index into
    /// FontBuffers::curves).
    uint32_t band_data_offset;

    uint32_t _pad;
};
static_assert(sizeof(GlyphRecord) == 32, "GlyphRecord must be 32 bytes (std430-friendly)");

/**
 * @brief Owns the per-font GPU-bound data for analytic Bezier text rendering.
 *
 * Three flat, append-only buffers:
 *  - curves: every glyph's quadratic Bezier curves, packed back-to-back
 *  - bands : per-glyph band acceleration data, packed back-to-back
 *  - glyphs: per-glyph metadata indexed by internal glyph index
 *
 * Glyphs are baked lazily. Repeated requests for the same FreeType glyph_id
 * return the same internal index without re-baking.
 *
 * Buffers grow by reallocation as needed (vector handles that). Any
 * append marks the FontBuffers dirty so the renderer can re-upload.
 *
 * No GPU dependency; this is pure CPU storage. Upload is the renderer's job.
 */
class FontBuffers
{
public:
    static constexpr uint32_t INVALID_INDEX = ~0u;

    /**
     * @brief Bakes a glyph if not already present, returns its internal index.
     *
     * The internal index is dense and stable for the lifetime of the
     * FontBuffers (assigned sequentially as new glyphs are added). Use it as
     * the index into the glyph table on the GPU.
     *
     * Returns INVALID_INDEX if the baker fails (cubic outline, FreeType error).
     * Empty glyphs (whitespace) DO get an internal index, but their curve_count
     * is 0 and the layout code can short-circuit on that.
     */
    uint32_t ensure_glyph(FT_Face face, uint32_t freetype_glyph_id);

    /// Look up a previously baked glyph; returns INVALID_INDEX if not present.
    uint32_t find_glyph(uint32_t freetype_glyph_id) const;

    /// Returns the glyph record for an internal index, or nullptr if invalid.
    const GlyphRecord* glyph_record(uint32_t internal_index) const;

    // Buffer access for upload.
    const QuadCurve* curves() const { return curves_.data(); }
    size_t curves_count() const { return curves_.size(); }
    size_t curves_bytes() const { return curves_.size() * sizeof(QuadCurve); }

    const uint32_t* bands() const { return bands_.data(); }
    size_t bands_count() const { return bands_.size(); }
    size_t bands_bytes() const { return bands_.size() * sizeof(uint32_t); }

    const GlyphRecord* glyphs() const { return glyphs_.data(); }
    size_t glyphs_count() const { return glyphs_.size(); }
    size_t glyphs_bytes() const { return glyphs_.size() * sizeof(GlyphRecord); }

    bool curves_dirty() const { return curves_dirty_; }
    bool bands_dirty() const { return bands_dirty_; }
    bool glyphs_dirty() const { return glyphs_dirty_; }
    void clear_curves_dirty() { curves_dirty_ = false; }
    void clear_bands_dirty() { bands_dirty_ = false; }
    void clear_glyphs_dirty() { glyphs_dirty_ = false; }

    void clear();

private:
    GlyphBaker baker_;
    BakedGlyph scratch_;

    vector<QuadCurve> curves_;
    vector<uint32_t> bands_;
    vector<GlyphRecord> glyphs_;

    // FreeType glyph_id -> internal dense index. No velk alternative; the
    // existing font_atlas uses std::unordered_map for the same purpose.
    std::unordered_map<uint32_t, uint32_t> id_to_index_;

    // Per-section dirty flags. Each grows independently as glyphs are baked
    // and the renderer wraps each section in its own IBuffer with its own
    // dirty cycle.
    bool curves_dirty_ = false;
    bool bands_dirty_ = false;
    bool glyphs_dirty_ = false;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_FONT_BUFFERS_H
