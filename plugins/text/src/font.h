#ifndef VELK_UI_TEXT_FONT_H
#define VELK_UI_TEXT_FONT_H

#include "font_buffers.h"

#include <velk/ext/object.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <ft2build.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/plugin.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>

namespace velk::ui::impl {

class IFontInternal : public Interface<IFontInternal>
{
public:
    /// Opens the face from @p bytes. Returns false if FreeType or HarfBuzz
    /// cannot use them; the font then reports exists() == false.
    virtual bool init(string_view uri, vector<uint8_t> bytes) = 0;
};

/**
 * @brief Font implementation: FreeType outline source + HarfBuzz shaper +
 *        FontBuffers, published into the shared text arenas.
 *
 * Created by the "font" resource decoder (`font:<inner_uri>`).
 *
 * No glyph atlas. Glyph outlines are extracted lazily by the GlyphBaker
 * (via FontBuffers::ensure_glyph) and packed into three runs (curves, bands,
 * glyph table), each a region of a shared arena (set = 1 slots 7 to 9). The
 * text material carries the three element bases, which the slug shader adds
 * to its own indices.
 *
 * Font is not an ISurface: there are no pixels to bind.
 */
class Font : public ::velk::ext::Object<Font, IFont, IResource, IFontInternal>
{
public:
    VELK_CLASS_UID(ClassId::Font, "Font");

    Font();
    ~Font() override;

    // IFontInternal
    bool init(string_view uri, vector<uint8_t> bytes) override;

    // IResource
    string_view uri() const override { return uri_; }
    bool exists() const override { return loaded_; }
    int64_t size() const override { return static_cast<int64_t>(font_data_.size()); }
    bool is_persistent() const override { return persistent_; }
    void set_persistent(bool value) override { persistent_ = value; }

    // IFont
    float shape_text(string_view text, vector<IFont::GlyphPosition>& out) override;
    GlyphInfo ensure_glyph(uint32_t glyph_id) override;
    void layout_text(string_view text, float font_size, TextLayout mode,
                     float available_width, TextLayoutResult& out) override;

    void ensure_gpu_data(IRenderContext& ctx) override;
    uint32_t curve_base() const override { return curve_base_; }
    uint32_t band_base()  const override { return band_base_; }
    uint32_t glyph_base() const override { return glyph_base_; }
    IMaterial::Ptr get_material()   const override { return text_material_; }

private:
    void init_buffers();

    /// Writes @p size bytes of @p data into @p buf, creating it from @p arena
    /// on first use. Returns the element base the shader indexes from. Growth
    /// is handled inside the buffer: it takes a fresh region and frees the old
    /// one (deferred), rather than overwriting under a frame that may still be
    /// reading.
    static uint32_t upload_section(IGpuArena* arena, IBuffer::Ptr& buf,
                                   const void* data, uint64_t size);

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

    string uri_;
    bool loaded_ = false;
    bool persistent_ = false;
    vector<uint8_t> font_data_;
    FT_Library ft_library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    hb_buffer_t* hb_buffer_ = nullptr;

    FontBuffers font_buffers_;

    /// This font's glyph data in the shared text arenas (set = 1 slots 7-9),
    /// as arena-backed IBuffers, plus the sizes last uploaded so growth is
    /// detected without reading back write-only memory.
    IBuffer::Ptr curve_buffer_;
    IBuffer::Ptr band_buffer_;
    IBuffer::Ptr glyph_buffer_;
    uint64_t curve_bytes_ = 0;
    uint64_t band_bytes_ = 0;
    uint64_t glyph_bytes_ = 0;
    uint32_t curve_base_ = 0;
    uint32_t band_base_ = 0;
    uint32_t glyph_base_ = 0;

    IMaterial::Ptr text_material_;
};

} // namespace velk::ui::impl

#endif // VELK_UI_TEXT_FONT_H
