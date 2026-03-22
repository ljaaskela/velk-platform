#ifndef VELK_UI_TEXT_FONT_ATLAS_H
#define VELK_UI_TEXT_FONT_ATLAS_H

#include <velk-ui/interface/intf_font.h>
#include <velk/api/math_types.h>
#include <velk/vector.h>

#include <cstdint>
#include <unordered_map>

namespace velk_ui {

struct AtlasRect
{
    uint32_t x, y, w, h;
    float bearing_x, bearing_y;
};

class GlyphAtlas
{
public:
    GlyphAtlas(uint32_t width = 1024, uint32_t height = 1024);

    const AtlasRect* ensure_glyph(IFont& font, uint32_t glyph_id);

    const uint8_t* get_pixels() const { return pixels_.data(); }
    uint32_t get_width() const { return width_; }
    uint32_t get_height() const { return height_; }
    bool is_dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    void clear();

private:
    uint32_t width_;
    uint32_t height_;
    velk::vector<uint8_t> pixels_;
    std::unordered_map<uint32_t, AtlasRect> glyphs_;
    bool dirty_ = false;

    // Row-based packer state
    uint32_t cursor_x_ = 0;
    uint32_t cursor_y_ = 0;
    uint32_t row_height_ = 0;
};

} // namespace velk_ui

#endif // VELK_UI_TEXT_FONT_ATLAS_H
