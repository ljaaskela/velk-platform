#ifndef VELK_UI_TEXT_FONT_H
#define VELK_UI_TEXT_FONT_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <ft2build.h>
#include <velk-render/ext/gpu_resource_mixin.h>
#include <velk-render/interface/intf_texture.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/plugin.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>

#include "font_atlas.h"

namespace velk::ui {

class Font : public ::velk::ext::Object<Font, IFont, ITexture>, public ::velk::ext::GpuResourceMixin
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
    const GlyphRect* ensure_glyph(uint32_t glyph_id) override;
    uint32_t get_atlas_width() const override;
    uint32_t get_atlas_height() const override;

    // ITexture
    int width() const override { return static_cast<int>(atlas_.get_width()); }
    int height() const override { return static_cast<int>(atlas_.get_height()); }
    PixelFormat format() const override { return PixelFormat::R8; }
    const uint8_t* get_pixels() const override { return atlas_.get_pixels(); }
    bool is_dirty() const override { return atlas_.is_dirty(); }
    void clear_dirty() override { atlas_.clear_dirty(); }

    // IGpuResource
    void add_gpu_resource_observer(IGpuResourceObserver* obs) override
    {
        GpuResourceMixin::add_observer(obs);
    }
    void remove_gpu_resource_observer(IGpuResourceObserver* obs) override
    {
        GpuResourceMixin::remove_observer(obs);
    }

private:
    vector<uint8_t> font_data_;
    FT_Library ft_library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    hb_buffer_t* hb_buffer_ = nullptr;
    GlyphAtlas atlas_;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_FONT_H
