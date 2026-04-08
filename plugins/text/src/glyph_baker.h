#ifndef VELK_UI_TEXT_GLYPH_BAKER_H
#define VELK_UI_TEXT_GLYPH_BAKER_H

#include <velk/api/math_types.h>
#include <velk/vector.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cstdint>

namespace velk::ui {

/**
 * @brief A single quadratic Bezier curve in glyph-local space.
 *
 * Quadratic curves are p0 -> p1 -> p2 with p1 as the control point.
 * Straight line segments are stored as degenerate quadratics where p1
 * is the midpoint of p0 and p2; this lets the inside-test treat all
 * segments uniformly without a separate code path.
 */
struct QuadCurve
{
    vec2 p0;
    vec2 p1;
    vec2 p2;
};

/**
 * @brief Output of baking one glyph for analytic Bezier coverage rendering.
 *
 * Holds the glyph's quadratic outline plus a band acceleration structure
 * used by the shader to skip irrelevant curves during the inside test.
 *
 * In-memory format notes:
 *  - Curves are stored in their full (p0, p1, p2) form. The eventual GPU
 *    wire format may exploit shared endpoints between consecutive curves
 *    on a contour for compactness; that packing is the packer's job and
 *    is intentionally not done here. Correctness first.
 *  - Curves are normalized to [0, 1] x [0, 1] over the glyph's own bbox
 *    so that band assignment can use uniform divisions independent of
 *    glyph size or font units.
 *  - The bbox is reported in raw FreeType font units (face em coordinates,
 *    obtained with FT_LOAD_NO_SCALE). Layout code multiplies by the
 *    pixels-per-em scale to convert to pixels.
 *  - Bands are stored as flat curve-index lists with prefix-sum offsets:
 *    band i covers indices [offsets[i], offsets[i + 1]) in the flat list.
 *  - Within each band, curves are sorted by their max coordinate on the
 *    OTHER axis (max x for horizontal bands, max y for vertical bands),
 *    descending. The shader walks the band until that key falls below the
 *    pixel coordinate and then early-exits.
 */
struct BakedGlyph
{
    static constexpr uint32_t BAND_COUNT = 8;

    /// Glyph bbox in raw FreeType font units (em coordinates).
    vec2 bbox_min;
    vec2 bbox_max;

    /// Curves in normalized glyph space (each component in [0, 1]).
    vector<QuadCurve> curves;

    /// Horizontal bands: stripes along y. Band i covers y in [i / N, (i + 1) / N].
    vector<uint16_t> h_band_curves;
    uint16_t h_band_offsets[BAND_COUNT + 1]{};

    /// Vertical bands: stripes along x. Band i covers x in [i / N, (i + 1) / N].
    vector<uint16_t> v_band_curves;
    uint16_t v_band_offsets[BAND_COUNT + 1]{};
};

/**
 * @brief Bakes a single glyph from a FreeType face into a BakedGlyph.
 *
 * Stateless apart from internal scratch buffers reused across calls; create
 * one and feed it glyphs lazily as a TextVisual encounters them.
 *
 * Only quadratic outlines are supported (TTF, OpenType-with-TT outlines).
 * Cubic outlines (CFF, OpenType-with-PS outlines) cause CubicNotSupported
 * to be returned. Inter is TTF, so this covers v1.
 */
class GlyphBaker
{
public:
    enum class Result
    {
        Ok,
        Empty,             ///< No contours (e.g. whitespace). out is left valid but empty.
        CubicNotSupported, ///< Outline contained a cubic segment.
        FreeTypeError      ///< FT_Load_Glyph or FT_Outline_Decompose failed.
    };

    /// Bake glyph_id from face into out, reusing out's existing storage when possible.
    Result bake(FT_Face face, uint32_t glyph_id, BakedGlyph& out);
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_GLYPH_BAKER_H
