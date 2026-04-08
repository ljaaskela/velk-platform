#include "font.h"

#include "embedded/inter_regular.h"

#include <velk/api/state.h>

#include <cstring>

namespace velk::ui {

Font::Font() = default;

Font::~Font()
{
    if (hb_buffer_) {
        hb_buffer_destroy(hb_buffer_);
        hb_buffer_ = nullptr;
    }
    if (hb_font_) {
        hb_font_destroy(hb_font_);
        hb_font_ = nullptr;
    }
    if (ft_face_) {
        FT_Done_Face(ft_face_);
        ft_face_ = nullptr;
    }
    if (ft_library_) {
        FT_Done_FreeType(ft_library_);
        ft_library_ = nullptr;
    }
}

void Font::init_buffers()
{
    curve_buffer_ = FontGpuBuffer::make(&font_buffers_, FontGpuBuffer::Role::Curves);
    band_buffer_  = FontGpuBuffer::make(&font_buffers_, FontGpuBuffer::Role::Bands);
    glyph_buffer_ = FontGpuBuffer::make(&font_buffers_, FontGpuBuffer::Role::Glyphs);
}

bool Font::init_from_memory(const uint8_t* data, uint32_t size)
{
    // Keep a copy: FreeType requires font data to stay alive
    font_data_.resize(size);
    std::memcpy(font_data_.data(), data, size);

    if (FT_Init_FreeType(&ft_library_) != 0) {
        return false;
    }

    if (FT_New_Memory_Face(
            ft_library_, font_data_.data(), static_cast<FT_Long>(font_data_.size()), 0, &ft_face_) != 0) {
        return false;
    }

    // Configure FreeType so that ppem == units_per_em. With dpi = 72,
    // ppem = char_size_pts, so char_size_pts = units_per_em. The 26.6 fixed
    // value is units_per_em * 64. This makes hb_ft_font_create set up the
    // HarfBuzz font with a scale that returns shaping advances in font
    // units (specifically in 1/64 of font units, which we then divide by
    // 64 in shape_text). Glyph baking still uses FT_LOAD_NO_SCALE so it
    // ignores this setting and reads raw outline coordinates.
    const FT_Long upem = ft_face_->units_per_EM;
    const FT_F26Dot6 reference_char_size = static_cast<FT_F26Dot6>(upem * 64);
    if (FT_Set_Char_Size(ft_face_, 0, reference_char_size, 72, 72) != 0) {
        return false;
    }

    hb_font_ = hb_ft_font_create(ft_face_, nullptr);
    if (!hb_font_) {
        return false;
    }

    hb_buffer_ = hb_buffer_create();
    if (!hb_buffer_) {
        return false;
    }

    init_buffers();

    // Read design-unit metrics directly from the face. These are constants
    // for the lifetime of the font; the visual scales them per-call.
    if (auto writer = write_state<IFont>(this)) {
        writer->units_per_em = static_cast<float>(upem);
        writer->ascender    = static_cast<float>(ft_face_->ascender);
        writer->descender   = static_cast<float>(ft_face_->descender);
        writer->line_height = static_cast<float>(ft_face_->height);
    }

    return true;
}

bool Font::init_default()
{
    return init_from_memory(embedded::inter_regular_ttf, embedded::inter_regular_ttf_size);
}

float Font::shape_text(string_view text, vector<IFont::GlyphPosition>& out)
{
    out.clear();

    if (!hb_font_ || !hb_buffer_ || text.empty()) {
        return 0.f;
    }

    hb_buffer_reset(hb_buffer_);
    hb_buffer_add_utf8(hb_buffer_, text.data(), static_cast<int>(text.size()), 0, -1);
    hb_buffer_set_direction(hb_buffer_, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buffer_, HB_SCRIPT_LATIN);
    hb_buffer_set_language(hb_buffer_, hb_language_from_string("en", -1));

    hb_shape(hb_font_, hb_buffer_, nullptr, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(hb_buffer_, &glyph_count);
    hb_glyph_position_t* glyph_pos = hb_buffer_get_glyph_positions(hb_buffer_, &glyph_count);

    float total_advance = 0.f;
    float scale = 1.f / 64.f;

    for (unsigned int i = 0; i < glyph_count; ++i) {
        GlyphPosition gp;
        gp.glyph_id = glyph_info[i].codepoint;
        gp.offset.x = static_cast<float>(glyph_pos[i].x_offset) * scale;
        gp.offset.y = static_cast<float>(glyph_pos[i].y_offset) * scale;
        gp.advance.x = static_cast<float>(glyph_pos[i].x_advance) * scale;
        gp.advance.y = static_cast<float>(glyph_pos[i].y_advance) * scale;
        out.push_back(gp);
        total_advance += gp.advance.x;
    }

    return total_advance;
}

IFont::GlyphInfo Font::ensure_glyph(uint32_t glyph_id)
{
    GlyphInfo info{};
    info.internal_index = FontBuffers::INVALID_INDEX;

    if (!ft_face_) {
        return info;
    }

    uint32_t idx = font_buffers_.ensure_glyph(ft_face_, glyph_id);
    if (idx == FontBuffers::INVALID_INDEX) {
        return info;
    }

    const GlyphRecord* rec = font_buffers_.glyph_record(idx);
    if (!rec) {
        return info;
    }

    info.internal_index = idx;
    info.bbox_min = rec->bbox_min;
    info.bbox_max = rec->bbox_max;
    info.empty = (rec->curve_count == 0);
    return info;
}

} // namespace velk::ui
