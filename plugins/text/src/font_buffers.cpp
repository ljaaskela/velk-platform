#include "font_buffers.h"

namespace velk::ui {

namespace {

// Append the prefix-sum offsets and the flat curve-index list for one axis
// of a baked glyph into the band buffer (as uint32_t entries).
void append_axis(
    velk::vector<uint32_t>& bands,
    const uint16_t (&offsets)[BakedGlyph::BAND_COUNT + 1],
    const velk::vector<uint16_t>& flat)
{
    for (uint32_t i = 0; i <= BakedGlyph::BAND_COUNT; ++i) {
        bands.push_back(static_cast<uint32_t>(offsets[i]));
    }
    for (size_t i = 0; i < flat.size(); ++i) {
        bands.push_back(static_cast<uint32_t>(flat[i]));
    }
}

} // namespace

uint32_t FontBuffers::ensure_glyph(FT_Face face, uint32_t freetype_glyph_id)
{
    auto it = id_to_index_.find(freetype_glyph_id);
    if (it != id_to_index_.end()) {
        return it->second;
    }

    auto result = baker_.bake(face, freetype_glyph_id, scratch_);

    GlyphRecord rec{};
    rec.bbox_min = scratch_.bbox_min;
    rec.bbox_max = scratch_.bbox_max;

    if (result == GlyphBaker::Result::Ok) {
        rec.curve_offset = static_cast<uint32_t>(curves_.size());
        rec.curve_count = static_cast<uint32_t>(scratch_.curves.size());
        rec.band_data_offset = static_cast<uint32_t>(bands_.size());

        // Append curves.
        for (size_t i = 0; i < scratch_.curves.size(); ++i) {
            curves_.push_back(scratch_.curves[i]);
        }

        // Append band data: h offsets, h indices, v offsets, v indices.
        append_axis(bands_, scratch_.h_band_offsets, scratch_.h_band_curves);
        append_axis(bands_, scratch_.v_band_offsets, scratch_.v_band_curves);
    } else if (result == GlyphBaker::Result::Empty) {
        // Whitespace etc: a record with curve_count == 0. The layout code
        // skips drawing for these but still needs to advance the pen, which
        // it does from HarfBuzz advance, not from this record.
        rec.curve_offset = 0;
        rec.curve_count = 0;
        rec.band_data_offset = 0;
    } else {
        // CubicNotSupported / FreeTypeError. Don't allocate an entry; signal
        // failure to the caller so it can substitute a fallback glyph.
        return INVALID_INDEX;
    }

    const uint32_t internal_index = static_cast<uint32_t>(glyphs_.size());
    glyphs_.push_back(rec);
    id_to_index_[freetype_glyph_id] = internal_index;

    // Every successful bake appends to glyphs_; only Ok bakes also append to
    // curves_ / bands_. Mark whichever sections actually grew.
    glyphs_dirty_ = true;
    if (rec.curve_count > 0) {
        curves_dirty_ = true;
        bands_dirty_ = true;
    }
    return internal_index;
}

uint32_t FontBuffers::find_glyph(uint32_t freetype_glyph_id) const
{
    auto it = id_to_index_.find(freetype_glyph_id);
    return (it == id_to_index_.end()) ? INVALID_INDEX : it->second;
}

const GlyphRecord* FontBuffers::glyph_record(uint32_t internal_index) const
{
    if (internal_index >= glyphs_.size()) {
        return nullptr;
    }
    return &glyphs_[internal_index];
}

void FontBuffers::clear()
{
    curves_.clear();
    bands_.clear();
    glyphs_.clear();
    id_to_index_.clear();
    curves_dirty_ = true;
    bands_dirty_ = true;
    glyphs_dirty_ = true;
}

} // namespace velk::ui
