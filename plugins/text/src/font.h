#ifndef VELK_UI_TEXT_FONT_H
#define VELK_UI_TEXT_FONT_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <ft2build.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/plugin.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>

namespace velk::ui {

class Font : public ::velk::ext::Object<Font, IFont>
{
public:
    VELK_CLASS_UID(ClassId::Font, "Font");

    Font();
    ~Font() override;

    bool init_from_memory(const uint8_t* data, uint32_t size);

    // IFont
    bool init_default() override;
    bool set_size(float size_px) override;
    float shape_text(string_view text, vector<IFont::GlyphPosition>& out) override;
    GlyphBitmap rasterize_glyph(uint32_t glyph_id) override;

private:
    vector<uint8_t> font_data_;
    FT_Library ft_library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    hb_buffer_t* hb_buffer_ = nullptr;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_FONT_H
