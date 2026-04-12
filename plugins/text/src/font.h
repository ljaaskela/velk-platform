#ifndef VELK_UI_TEXT_FONT_H
#define VELK_UI_TEXT_FONT_H

#include "font_buffers.h"

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <ft2build.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/plugin.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>

namespace velk::ui::impl {

/**
 * @brief Font implementation: FreeType outline source + HarfBuzz shaper +
 *        FontBuffers + three FontGpuBuffer wrappers.
 *
 * No glyph atlas. Glyph outlines are extracted lazily by the GlyphBaker
 * (via FontBuffers::ensure_glyph) and packed into three GPU buffers
 * (curves, bands, glyph table) that the renderer uploads via the
 * IBuffer path. The text material reads each buffer's GPU address inside
 * `write_gpu_data` and emits them as buffer references the slug shader
 * can dereference.
 *
 * Font is no longer an ITexture: there are no pixels to bind.
 */
class Font : public ::velk::ext::Object<Font, IFont>
{
public:
    VELK_CLASS_UID(ClassId::Font, "Font");

    Font();
    ~Font() override;

    bool init_from_memory(const uint8_t* data, uint32_t size);

    // IFont
    bool init_default() override;
    float shape_text(string_view text, vector<IFont::GlyphPosition>& out) override;
    GlyphInfo ensure_glyph(uint32_t glyph_id) override;
    void layout_text(string_view text, float font_size, TextLayout mode,
                     float available_width, TextLayoutResult& out) override;

    IBuffer::Ptr get_curve_buffer() const override { return curve_buffer_; }
    IBuffer::Ptr get_band_buffer()  const override { return band_buffer_; }
    IBuffer::Ptr get_glyph_buffer() const override { return glyph_buffer_; }
    IMaterial::Ptr get_material()   const override { return text_material_; }

private:
    void init_buffers();

    void layout_line_glyphs(string_view text, float scale, float ascender_px,
                            float baseline_y, TextLayoutResult& out);
    void layout_single_line(string_view text, float scale, float ascender_px,
                            float line_height_px, float available_width,
                            TextLayoutResult& out);
    void layout_multi_line(string_view text, float scale, float ascender_px,
                           float line_height_px, TextLayoutResult& out);
    void layout_word_wrap(string_view text, float scale, float ascender_px,
                          float line_height_px, float available_width,
                          TextLayoutResult& out);

    vector<uint8_t> font_data_;
    FT_Library ft_library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    hb_buffer_t* hb_buffer_ = nullptr;

    FontBuffers font_buffers_;
    IBuffer::Ptr curve_buffer_;
    IBuffer::Ptr band_buffer_;
    IBuffer::Ptr glyph_buffer_;
    IMaterial::Ptr text_material_;
};

} // namespace velk::ui::impl

#endif // VELK_UI_TEXT_FONT_H
