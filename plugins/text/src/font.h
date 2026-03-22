#ifndef VELK_UI_TEXT_FONT_H
#define VELK_UI_TEXT_FONT_H

#include <velk-ui/interface/intf_font.h>
#include <velk/ext/object.h>
#include <velk/vector.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

namespace velk_ui {

class Font : public velk::ext::Object<Font, IFont>
{
public:
    VELK_CLASS_UID("f4a1b2c3-d5e6-4f78-9a0b-c1d2e3f4a5b6", "Font");

    Font();
    ~Font() override;

    bool init_from_memory(const uint8_t* data, uint32_t size);

    // IFont
    bool set_size(float size_px) override;
    float shape_text(velk::string_view text, velk::vector<IFont::GlyphPosition>& out) override;
    GlyphBitmap rasterize_glyph(uint32_t glyph_id) override;

private:
    velk::vector<uint8_t> font_data_;
    FT_Library ft_library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    hb_buffer_t* hb_buffer_ = nullptr;
};

} // namespace velk_ui

#endif // VELK_UI_TEXT_FONT_H
