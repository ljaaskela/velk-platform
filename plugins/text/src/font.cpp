#include "font.h"

#include "embedded/inter_regular.h"

#include <velk/api/state.h>

#include <cstring>

namespace velk::ui {

Font::Font() = default;

Font::~Font()
{
    notify_gpu_resource_destroyed(this);
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

    hb_font_ = hb_ft_font_create(ft_face_, nullptr);
    if (!hb_font_) {
        return false;
    }

    hb_buffer_ = hb_buffer_create();
    if (!hb_buffer_) {
        return false;
    }

    return true;
}

bool Font::init_default()
{
    return init_from_memory(embedded::inter_regular_ttf, embedded::inter_regular_ttf_size);
}

bool Font::set_size(float size_px)
{
    if (!ft_face_) {
        return false;
    }

    // FreeType uses 1/64th of a point; at 72 DPI, 1pt = 1px
    FT_F26Dot6 size_26_6 = static_cast<FT_F26Dot6>(size_px * 64.f);
    if (FT_Set_Char_Size(ft_face_, 0, size_26_6, 72, 72) != 0) {
        return false;
    }

    // Recreate HarfBuzz font to pick up the new size
    if (hb_font_) {
        hb_font_destroy(hb_font_);
    }
    hb_font_ = hb_ft_font_create(ft_face_, nullptr);
    if (!hb_font_) {
        return false;
    }

    // Update metrics in state
    auto writer = write_state<IFont>(this);
    if (writer) {
        float scale = 1.f / 64.f;
        writer->ascender = static_cast<float>(ft_face_->size->metrics.ascender) * scale;
        writer->descender = static_cast<float>(ft_face_->size->metrics.descender) * scale;
        writer->line_height = static_cast<float>(ft_face_->size->metrics.height) * scale;
        writer->size_px = size_px;
    }

    return true;
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

IFont::GlyphBitmap Font::rasterize_glyph(uint32_t glyph_id)
{
    GlyphBitmap result{};

    if (!ft_face_) {
        return result;
    }

    if (FT_Load_Glyph(ft_face_, glyph_id, FT_LOAD_RENDER) != 0) {
        return result;
    }

    FT_GlyphSlot slot = ft_face_->glyph;
    result.data = slot->bitmap.buffer;
    result.width = slot->bitmap.width;
    result.height = slot->bitmap.rows;
    result.bearing.x = static_cast<float>(slot->bitmap_left);
    result.bearing.y = static_cast<float>(slot->bitmap_top);

    return result;
}

const IFont::GlyphRect* Font::ensure_glyph(uint32_t glyph_id)
{
    return atlas_.ensure_glyph(*this, glyph_id);
}

uint32_t Font::get_atlas_width() const { return atlas_.get_width(); }
uint32_t Font::get_atlas_height() const { return atlas_.get_height(); }


} // namespace velk::ui
