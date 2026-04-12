#ifndef VELK_UI_INTF_FONT_H
#define VELK_UI_INTF_FONT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_material.h>

#include <velk-ui/types.h>

#include <cstdint>

namespace velk::ui {

/**
 * @brief A font that shapes text and exposes glyph outline data for
 *        analytic Bezier rendering.
 *
 * Conceptually a Font owns:
 *  - A FreeType face for outline extraction.
 *  - A HarfBuzz shaper for converting text strings into glyph runs.
 *  - Three GPU-uploadable buffers (curves, bands, glyph table) that
 *    accumulate as glyphs are baked on demand.
 *
 * Glyphs are baked lazily: the first time a glyph_id is referenced, the
 * font extracts its outline, packs the curves and bands, and appends the
 * data to the GPU buffers. Subsequent references return the cached entry.
 *
 * The buffers are exposed as IBuffer pointers for the renderer to upload
 * and for materials to read GPU virtual addresses from.
 */
class IFont : public Interface<IFont>
{
public:
    /// Position of one glyph in a shaped text run, in font units.
    /// The visual scales these to pixels using `font_size / units_per_em`.
    struct GlyphPosition
    {
        uint32_t glyph_id;
        vec2 offset;  // font units, relative to pen position
        vec2 advance; // font units, how far to move the pen
    };

    /// Cached layout metadata for a baked glyph.
    struct GlyphInfo
    {
        /// Index into the font's glyph table buffer. Passed to the shader
        /// per text instance so it can locate the glyph's curves and bands.
        uint32_t internal_index;

        /// Glyph bbox in font units (FreeType em coordinates). Used by
        /// layout code to size and position the rendered quad.
        vec2 bbox_min;
        vec2 bbox_max;

        /// True if the glyph has no curves (whitespace etc). Layout code
        /// should advance the pen but skip emitting a draw quad.
        bool empty;
    };

    /// A glyph positioned in pixel space, ready for rendering.
    struct PositionedGlyph
    {
        vec2 pos;              ///< Pixel position of the glyph quad.
        vec2 size;             ///< Pixel size of the glyph quad.
        uint32_t glyph_index;  ///< Index into the glyph table buffer (for the shader).
    };

    /// One line within a laid-out text block.
    struct LayoutLine
    {
        uint32_t first_glyph;  ///< Index into TextLayoutResult::glyphs.
        uint32_t glyph_count;
        float width;           ///< Pixel width of this line.
    };

    /// Result of layout_text: fully positioned glyphs organized into lines.
    struct TextLayoutResult
    {
        vector<PositionedGlyph> glyphs;
        vector<LayoutLine> lines;
        float total_width;   ///< Max line width across all lines.
        float total_height;  ///< Total height of all lines.
        float line_height;   ///< Pixel line height (for vertical spacing).
    };

    /// Font is scale-independent. All metrics are in raw font units (the
    /// FreeType face's design units). Consumers (text visuals) multiply by
    /// `requested_size_px / units_per_em` to convert to pixel space, so the
    /// same Font instance can serve any pixel size without re-baking.
    VELK_INTERFACE(
        (RPROP, float, ascender, 0.f),     ///< Ascender in font units.
        (RPROP, float, descender, 0.f),    ///< Descender in font units.
        (RPROP, float, line_height, 0.f),  ///< Line height in font units.
        (RPROP, float, units_per_em, 0.f)  ///< Font units per em (constant per face).
    )

    /** @brief Initializes the font from a built-in default (embedded Inter Regular). */
    virtual bool init_default() = 0;

    /** @brief Shapes a text string into a sequence of positioned glyphs (in font units). */
    virtual float shape_text(string_view text, vector<GlyphPosition>& out) = 0;

    /**
     * @brief Bakes a glyph if not already present, returning its layout
     *        metadata.
     *
     * The returned `GlyphInfo` is valid until the font is destroyed; the
     * internal_index remains stable for the lifetime of the font and
     * indexes into the glyph table buffer that the renderer uploads.
     *
     * Returns a GlyphInfo with `empty = true` for whitespace and other
     * glyphs with no outline data.
     */
    virtual GlyphInfo ensure_glyph(uint32_t glyph_id) = 0;

    /// @name GPU-bound buffers
    /// The Font owns these. Their contents grow as glyphs are baked. The
    /// renderer uploads them, the text material reads their GPU addresses
    /// to bind them via buffer_reference in the slug coverage shader.
    /// @{
    virtual IBuffer::Ptr get_curve_buffer() const = 0;
    virtual IBuffer::Ptr get_band_buffer() const = 0;
    virtual IBuffer::Ptr get_glyph_buffer() const = 0;
    /// @}

    /**
     * @brief Lays out text into positioned glyphs organized into lines.
     *
     * Handles shaping, glyph baking, line splitting, word wrapping, and
     * ellipsis truncation. The result is in pixel space, ready for the
     * visual to apply alignment and color.
     *
     * @param text            The text to lay out.
     * @param font_size       Font size in pixels.
     * @param mode            Layout mode (SingleLine, MultiLine, WordWrap).
     * @param available_width Available width in pixels (0 = unconstrained).
     * @param out             Receives the positioned glyphs and line info.
     */
    virtual void layout_text(string_view text, float font_size, TextLayout mode,
                             float available_width, TextLayoutResult& out) = 0;

    /**
     * @brief Returns the rendering material for this font.
     *
     * The Font owns one IMaterial instance bound to its three GPU buffers.
     * Visuals consume the same material instance, which means visuals
     * sharing a font also share a draw call (the renderer batches by
     * material identity).
     */
    virtual IMaterial::Ptr get_material() const = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_FONT_H
